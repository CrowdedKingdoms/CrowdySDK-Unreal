#include "Replication/State/CrowdyStateCodec.h"

#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "CrowdyReplicationLog.h"
#include "HAL/UnrealMemory.h"     // FMemory (scratch value for change detection)
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/StructuredArchive.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "UObject/Class.h"        // UScriptStruct, STRUCT_NetSerializeNative
#include "UObject/UnrealType.h"   // FProperty, FStructProperty

namespace
{
	// An FMemoryReader whose ArMaxSerializeSize is pinned to the blob length so the engine's own
	// length-prefixed-container guard fires. A plain FMemoryReader leaves ArMaxSerializeSize == 0, which
	// GATES OFF the FString/FName load path's `(MaxSerializeSize > 0) && (SaveNum > MaxSerializeSize)`
	// self-protection (FString::SerializeItem in String.cpp.inl): the untrusted int32 length prefix would
	// then drive Str.Data.AddUninitialized() to a multi-GB allocation BEFORE the subsequent short char
	// read is detected, turning a tiny forged packet into a remote OOM. Setting a positive cap makes the
	// engine reject SaveNum > blob-size up front and the decode drops cleanly with no giant allocation.
	// SetLimitSize alone is insufficient: it caps TotalSize()/Serialize but not GetMaxSerializeSize(),
	// which is what the string path allocates against.
	class FCrowdyBoundedMemoryReader : public FMemoryReader
	{
	public:
		FCrowdyBoundedMemoryReader(const TArray<uint8>& InBytes, bool bIsPersistent)
			: FMemoryReader(InBytes, bIsPersistent)
		{
			// No length prefix inside this blob can legitimately exceed the blob itself, so the blob
			// size is the tightest correct cap.
			ArMaxSerializeSize = InBytes.Num();
		}
	};

	// Upper bound on the byte length of one quantized (net-serialized) value the decoder will accept
	// from an untrusted peer. The net serializers in scope (the FVector_NetQuantize family and similar
	// small structs) encode to a handful of bytes; a forged length past this cannot be allowed to drive
	// an unbounded scratch allocation, so it drops the delta. Far above any legitimate quantized value.
	constexpr int32 CrowdyStateMaxQuantizedBytes = 4096;

	// LEB128 unsigned varint. A u64 is at most 10 continuation groups (7 bits each), so a stream that
	// keeps the high bit set past the 10th byte is malformed and must be rejected rather than shifted
	// past the width of the value.
	constexpr int32 CrowdyStateMaxVarUIntBytes = 10;

	void WriteVarUInt(FArchive& Ar, uint64 Value)
	{
		do
		{
			uint8 Byte = static_cast<uint8>(Value & 0x7Fu);
			Value >>= 7;
			if (Value != 0)
			{
				Byte |= 0x80u;
			}
			Ar << Byte;
		}
		while (Value != 0);
	}

	// Reads one LEB128 varint. Returns false (and sets the archive error) on a truncated stream or an
	// over-wide encoding; the value is only valid when it returns true.
	bool ReadVarUInt(FArchive& Ar, uint64& OutValue)
	{
		OutValue = 0;
		uint32 Shift = 0;
		for (int32 ByteIndex = 0; ByteIndex < CrowdyStateMaxVarUIntBytes; ++ByteIndex)
		{
			if (Ar.AtEnd() || Ar.IsError())
			{
				Ar.SetError();
				return false;
			}
			uint8 Byte = 0;
			Ar << Byte;
			OutValue |= static_cast<uint64>(Byte & 0x7Fu) << Shift;
			if ((Byte & 0x80u) == 0)
			{
				return true;
			}
			Shift += 7;
		}

		// Ran past the maximum width without a terminating byte: an over-long (or forged) encoding.
		Ar.SetError();
		return false;
	}

	// Number of LEB128 bytes Value encodes to, used to size the two selector representations before
	// choosing the smaller. Matches WriteVarUInt's byte count exactly (a zero value is one byte).
	int32 VarUIntLen(uint64 Value)
	{
		int32 Len = 1;
		while (Value >= 0x80u)
		{
			Value >>= 7;
			++Len;
		}
		return Len;
	}

	// A property is quantized iff it is an FStructProperty whose UScriptStruct declares a native net
	// serializer. FStructProperty::NetSerializeItem FATAL-logs the "Deprecated code path" for any struct
	// that is NOT STRUCT_NetSerializeNative (verified in engine PropertyStruct.cpp), so encode and decode
	// MUST gate on this identically a plain FVector/FRotator has no net serializer and rides
	// SerializeItem, which is the correctness floor; quantization is opportunistic.
	bool IsNetQuantizedProperty(const FProperty* Prop)
	{
		const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		return StructProp != nullptr
			&& StructProp->Struct != nullptr
			&& (StructProp->Struct->StructFlags & STRUCT_NetSerializeNative) != 0;
	}

	// Serializes one value into Writer at its current position. A net-quantized struct is framed as a
	// length prefix plus its NetSerializeItem bytes so the bit-packed sub-encoding never disturbs the
	// main archive's byte position; everything else rides SerializeItem, byte-identical to the RPC plane.
	void EncodeValue(const FProperty* Prop, void* ValuePtr, FArchive& Writer)
	{
		if (IsNetQuantizedProperty(Prop))
		{
			TArray<uint8> Sub;
			{
				FMemoryWriter SubW(Sub, /*bIsPersistent=*/true);
				Prop->NetSerializeItem(SubW, /*Map*/nullptr, ValuePtr);
			}
			int32 Len = Sub.Num();
			Writer << Len;
			if (Len > 0)
			{
				Writer.Serialize(Sub.GetData(), Len);
			}
			return;
		}

		FStructuredArchiveFromArchive Adapter(Writer);
		Prop->SerializeItem(Adapter.GetSlot(), ValuePtr, nullptr);
	}

	// Reverses EncodeValue for one value. Returns false (dropping the delta) on any malformed framing:
	// a negative or oversized quantized length, a length past the bytes remaining, or a sub-archive that
	// erred. The length is untrusted, so both bounds are checked before the scratch read.
	bool DecodeValue(const FProperty* Prop, void* ValuePtr, FMemoryReader& Reader, const TArray<uint8>& Blob)
	{
		if (IsNetQuantizedProperty(Prop))
		{
			int32 Len = 0;
			Reader << Len;
			if (Reader.IsError())
			{
				return false;
			}

			const int64 Remaining = static_cast<int64>(Blob.Num()) - Reader.Tell();
			if (Len < 0 || Len > CrowdyStateMaxQuantizedBytes || static_cast<int64>(Len) > Remaining)
			{
				UE_LOG(LogCrowdyReplication, Warning,
					TEXT("CrowdyStateCodec::Decode: quantized value for '%s' has out-of-range length %d (remaining %lld); dropping."),
					*Prop->GetName(), Len, Remaining);
				return false;
			}

			TArray<uint8> Scratch;
			Scratch.SetNumUninitialized(Len);
			if (Len > 0)
			{
				Reader.Serialize(Scratch.GetData(), Len);
			}
			if (Reader.IsError())
			{
				return false;
			}

			FMemoryReader SubR(Scratch, /*bIsPersistent=*/true);
			Prop->NetSerializeItem(SubR, /*Map*/nullptr, ValuePtr);
			if (SubR.IsError())
			{
				UE_LOG(LogCrowdyReplication, Warning,
					TEXT("CrowdyStateCodec::Decode: quantized value for '%s' failed to net-deserialize; dropping."),
					*Prop->GetName());
				return false;
			}
			return true;
		}

		FStructuredArchiveFromArchive Adapter(Reader);
		Prop->SerializeItem(Adapter.GetSlot(), ValuePtr, nullptr);
		return !Reader.IsError();
	}
}

void FCrowdyStateCodec::Encode(const FCrowdyRepLayout& Layout, const void* Container, const TBitArray<>& Dirty,
	bool bKeyframe, TArray<uint8>& OutBlob)
{
	OutBlob.Reset();

	const int32 N = Layout.Properties.Num();

	// Collect the present layout indices in ascending order. A keyframe forces every slot present; a hot
	// delta takes exactly the set Dirty bits (bounded by N, so an over-long Dirty array cannot leak a
	// slot that does not exist in the layout).
	TArray<int32> Present;
	Present.Reserve(N);
	for (int32 Index = 0; Index < N; ++Index)
	{
		const bool bSet = bKeyframe || (Dirty.IsValidIndex(Index) && Dirty[Index]);
		if (bSet)
		{
			Present.Add(Index);
		}
	}

	FMemoryWriter Writer(OutBlob, /*bIsPersistent=*/true);

	uint8 Version = CrowdyStateBlobVersion;
	Writer << Version;

	// Size the two selector representations and pick the smaller. Bitmask is a fixed ceil(N/8) bytes;
	// the index-list is a varint count plus one varint per present index. Ties go to the bitmask (a
	// keyframe with all N present always favours it), which also keeps the all-set case compact.
	const int32 BitmaskBytes = (N + 7) / 8;
	int32 IndexListBytes = VarUIntLen(static_cast<uint64>(Present.Num()));
	for (int32 Index : Present)
	{
		IndexListBytes += VarUIntLen(static_cast<uint64>(Index));
	}

	const bool bUseIndexList = IndexListBytes < BitmaskBytes;
	uint8 SelectorMode = bUseIndexList ? 1 : 0;
	Writer << SelectorMode;

	if (bUseIndexList)
	{
		WriteVarUInt(Writer, static_cast<uint64>(Present.Num()));
		for (int32 Index : Present)
		{
			WriteVarUInt(Writer, static_cast<uint64>(Index));
		}
	}
	else
	{
		TArray<uint8> Mask;
		Mask.SetNumZeroed(BitmaskBytes);
		for (int32 Index : Present)
		{
			Mask[Index >> 3] |= static_cast<uint8>(1u << (Index & 7));
		}
		if (BitmaskBytes > 0)
		{
			Writer.Serialize(Mask.GetData(), BitmaskBytes);
		}
	}

	// Positional body: each present value in ascending index order. const_cast mirrors the RPC encoder
	// SerializeItem/NetSerializeItem take a non-const value pointer even when only reading.
	void* MutableContainer = const_cast<void*>(Container);
	for (int32 Index : Present)
	{
		const FProperty* Prop = Layout.Properties[Index].Property;
		if (!Prop)
		{
			continue;
		}
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(MutableContainer);
		EncodeValue(Prop, ValuePtr, Writer);
	}
}

bool FCrowdyStateCodec::Decode(const FCrowdyRepLayout& Layout, int64 IncomingLayoutHash, const TArray<uint8>& Blob,
	void* Container, TArray<int32>& OutChangedIndices)
{
	OutChangedIndices.Reset();

	const int32 N = Layout.Properties.Num();

	// The positional guard runs first, before any blob byte is touched, so a drifted peer's delta leaves
	// the target fully untouched rather than being misparsed by position.
	if (IncomingLayoutHash != Layout.LayoutHash)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("CrowdyStateCodec::Decode: layout hash %lld != local %lld; dropping delta."),
			IncomingLayoutHash, Layout.LayoutHash);
		return false;
	}

	if (Blob.Num() < 1)
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("CrowdyStateCodec::Decode: empty blob; dropping delta."));
		return false;
	}

	// Bounded reader: caps ArMaxSerializeSize so a forged FString/FName length prefix in the body cannot
	// drive an unbounded allocation before the short read is detected (see FCrowdyBoundedMemoryReader).
	FCrowdyBoundedMemoryReader Reader(Blob, /*bIsPersistent=*/true);

	uint8 Version = 0;
	Reader << Version;
	if (Version != CrowdyStateBlobVersion)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("CrowdyStateCodec::Decode: blob version %u != expected %u; dropping delta."),
			Version, CrowdyStateBlobVersion);
		return false;
	}

	if (Reader.AtEnd())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("CrowdyStateCodec::Decode: blob ends before the selector mode; dropping delta."));
		return false;
	}
	uint8 SelectorMode = 0;
	Reader << SelectorMode;
	if (SelectorMode != 0 && SelectorMode != 1)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("CrowdyStateCodec::Decode: unknown selector mode %u; dropping delta."), SelectorMode);
		return false;
	}

	// Resolve the present layout indices (ascending). Both selector paths reject anything out of range so
	// the value loop below only ever indexes a real layout slot.
	TArray<int32> Present;
	if (SelectorMode == 0)
	{
		const int32 BitmaskBytes = (N + 7) / 8;
		if (static_cast<int64>(Blob.Num()) - Reader.Tell() < BitmaskBytes)
		{
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("CrowdyStateCodec::Decode: blob too short for a %d-byte bitmask; dropping delta."), BitmaskBytes);
			return false;
		}
		TArray<uint8> Mask;
		Mask.SetNumUninitialized(BitmaskBytes);
		if (BitmaskBytes > 0)
		{
			Reader.Serialize(Mask.GetData(), BitmaskBytes);
		}
		if (Reader.IsError())
		{
			return false;
		}
		for (int32 Index = 0; Index < N; ++Index)
		{
			if ((Mask[Index >> 3] & static_cast<uint8>(1u << (Index & 7))) != 0)
			{
				Present.Add(Index);
			}
		}
	}
	else
	{
		uint64 Count = 0;
		if (!ReadVarUInt(Reader, Count))
		{
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("CrowdyStateCodec::Decode: malformed index-list count varint; dropping delta."));
			return false;
		}
		if (Count > static_cast<uint64>(N))
		{
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("CrowdyStateCodec::Decode: index-list count %llu exceeds layout size %d; dropping delta."),
				Count, N);
			return false;
		}

		Present.Reserve(static_cast<int32>(Count));
		int32 Previous = -1;
		for (uint64 Read = 0; Read < Count; ++Read)
		{
			uint64 Raw = 0;
			if (!ReadVarUInt(Reader, Raw))
			{
				UE_LOG(LogCrowdyReplication, Warning,
					TEXT("CrowdyStateCodec::Decode: malformed index varint; dropping delta."));
				return false;
			}
			// Strictly ascending and in range: rejecting duplicates, descending order, and out-of-range
			// indices keeps the positional body unambiguous and never indexes outside the layout.
			if (Raw >= static_cast<uint64>(N) || static_cast<int32>(Raw) <= Previous)
			{
				UE_LOG(LogCrowdyReplication, Warning,
					TEXT("CrowdyStateCodec::Decode: index %llu is out of range or not strictly ascending (prev %d); dropping delta."),
					Raw, Previous);
				return false;
			}
			Previous = static_cast<int32>(Raw);
			Present.Add(Previous);
		}
	}

	// Positional body: decode each present value in ascending index order into a scratch value, then write it
	// onto the live container and record the slot ONLY when it actually differs from the value already there.
	// "Changed" must mean "the value moved", not merely "present in the delta": the keyframe heartbeat (Phase 5)
	// re-sends every non-owner-only property on its interval, so recording every present slot would refire that
	// property's CrowdyOnRep on every heartbeat even when nothing moved. Matching UE RepNotify-on-change makes a
	// heartbeat idempotent. Decoding into scratch first also leaves the live value (and any heap it owns, e.g.
	// an FString) untouched on an unchanged slot and on a mid-body drop. Any short read or malformed value drops
	// the whole delta (return false); leading slots already found changed stay written, and the caller re-pulls.
	for (int32 Index : Present)
	{
		const FProperty* Prop = Layout.Properties[Index].Property;
		if (!Prop)
		{
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("CrowdyStateCodec::Decode: layout slot %d has no resolved property; dropping delta."), Index);
			return false;
		}

		// Scratch value the incoming bytes decode into. Its lifetime is this iteration
		// (Initialize -> decode -> compare/copy-on-change -> Destroy), so the live value is only touched on a
		// real change. Static arrays (ArrayDim > 1) are rejected at discovery, so GetSize() is one value's worth.
		void* Scratch = FMemory::Malloc(Prop->GetSize(), Prop->GetMinAlignment());
		Prop->InitializeValue(Scratch);

		const bool bDecoded = DecodeValue(Prop, Scratch, Reader, Blob);
		if (bDecoded)
		{
			void* LivePtr = Prop->ContainerPtrToValuePtr<void>(Container);
			if (!Prop->Identical(Scratch, LivePtr, PPF_None))
			{
				Prop->CopyCompleteValue(LivePtr, Scratch);
				OutChangedIndices.Add(Index);
			}
		}

		Prop->DestroyValue(Scratch);
		FMemory::Free(Scratch);

		if (!bDecoded)
		{
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("CrowdyStateCodec::Decode: ran out of bytes or bad value on '%s'; dropping delta."),
				*Prop->GetName());
			return false;
		}
	}

	// Trailing bytes mean the blob does not match the layout the sender claimed; drop rather than accept
	// a partially understood delta.
	if (Reader.Tell() != Blob.Num())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("CrowdyStateCodec::Decode: %lld unread byte(s) after the body (read %lld of %d); dropping delta."),
			static_cast<int64>(Blob.Num()) - Reader.Tell(), Reader.Tell(), Blob.Num());
		return false;
	}

	return true;
}

void FCrowdyStateCodec::EncodeChannelStateDelta(const FCrowdyStateDelta& Delta, TArray<uint8>& OutPayload)
{
	OutPayload.Reset();

	FMemoryWriter Writer(OutPayload, /*bIsPersistent=*/true);

	uint8 Tag = CrowdyChannelStateDeltaTag;
	Writer << Tag;
	uint8 Version = CrowdyChannelStateDeltaVersion;
	Writer << Version;

	// FMemoryWriter's operators handle each field, including the byte array. The Blob already carries its own
	// body version, so the channel header sits in front of the whole delta (mirrors EncodeChannelRpc).
	FCrowdyStateDelta Mutable = Delta;
	Writer << Mutable.ClassID;
	Writer << Mutable.EntityID;
	Writer << Mutable.SenderID;
	Writer << Mutable.LayoutHash;
	Writer << Mutable.Flags;
	Writer << Mutable.Blob;
}

bool FCrowdyStateCodec::DecodeChannelStateDelta(const TArray<uint8>& Payload, FCrowdyStateDelta& Out)
{
	// Minimum: tag + version.
	if (Payload.Num() < 2)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("DecodeChannelStateDelta: payload too short for a header; dropping."));
		return false;
	}

	// Bounded reader per the Phase-2 idiom (caps ArMaxSerializeSize so any FString/FName length prefix is
	// guarded). The delta header carries no strings, so the Blob is the only untrusted-length field, and its
	// bulk TArray<uint8> load is NOT gated by ArMaxSerializeSize on a non-net archive (verified in engine
	// Array.h: the 16MB guard is IsNetArchive-only) hence the explicit length bound below is the load-bearing
	// OOM protection here, matching how DecodeValue bounds a quantized sub-blob.
	FCrowdyBoundedMemoryReader Reader(Payload, /*bIsPersistent=*/true);

	uint8 Tag = 0;
	Reader << Tag;
	if (Tag != CrowdyChannelStateDeltaTag)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("DecodeChannelStateDelta: kind tag %u != expected %u; dropping."), Tag, CrowdyChannelStateDeltaTag);
		return false;
	}

	uint8 Version = 0;
	Reader << Version;
	if (Version != CrowdyChannelStateDeltaVersion)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("DecodeChannelStateDelta: version %u != expected %u; dropping."), Version, CrowdyChannelStateDeltaVersion);
		return false;
	}

	Reader << Out.ClassID;
	Reader << Out.EntityID;
	Reader << Out.SenderID;
	Reader << Out.LayoutHash;
	Reader << Out.Flags;
	if (Reader.IsError())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("DecodeChannelStateDelta: ran out of bytes decoding the header fields; dropping."));
		return false;
	}

	// Bound the Blob length against the bytes remaining BEFORE reading it, so a forged length prefix cannot
	// drive an unbounded allocation. `Writer << Blob` wrote an int32 count then the raw bytes, so reading the
	// int32 first and validating it reproduces that framing exactly.
	int32 BlobLen = 0;
	Reader << BlobLen;
	if (Reader.IsError())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("DecodeChannelStateDelta: ran out of bytes reading the Blob length; dropping."));
		return false;
	}
	const int64 Remaining = static_cast<int64>(Payload.Num()) - Reader.Tell();
	if (BlobLen < 0 || static_cast<int64>(BlobLen) > Remaining)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("DecodeChannelStateDelta: Blob length %d out of range (remaining %lld); dropping."),
			BlobLen, Remaining);
		return false;
	}

	Out.Blob.SetNumUninitialized(BlobLen);
	if (BlobLen > 0)
	{
		Reader.Serialize(Out.Blob.GetData(), BlobLen);
	}
	if (Reader.IsError())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("DecodeChannelStateDelta: ran out of bytes reading the Blob body; dropping."));
		return false;
	}

	// Trailing bytes mean the payload does not match the framing the sender claimed; drop rather than accept
	// a partially understood delta.
	if (Reader.Tell() != Payload.Num())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("DecodeChannelStateDelta: %lld unread byte(s) after the delta; dropping."),
			static_cast<int64>(Payload.Num()) - Reader.Tell());
		return false;
	}

	return true;
}
