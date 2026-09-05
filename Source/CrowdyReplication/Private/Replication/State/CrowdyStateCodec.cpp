#include "Replication/State/CrowdyStateCodec.h"

#include "Replication/CrowdyBoundedMemoryReader.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "CrowdyReplicationLog.h"
#include "HAL/UnrealMemory.h"     // FMemory (scratch value for change detection)
#include "Memory/MemoryView.h"    // MakeMemoryView (bounded view over a quantized sub-blob)
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/StructuredArchive.h"
#include "Serialization/StructuredArchiveAdapters.h"
#include "UObject/Class.h"        // UScriptStruct, STRUCT_NetSerializeNative
#include "UObject/UnrealType.h"   // FProperty, FStructProperty

namespace
{
	// Upper bound on the byte length of one quantized (net-serialized) value the decoder will accept
	// from an untrusted peer. The net serializers in scope (the FVector_NetQuantize family and similar
	// small structs) encode to a handful of bytes; a forged length past this cannot be allowed to drive
	// an unbounded scratch allocation, so it drops the delta. Far above any legitimate quantized value.
	constexpr int32 CrowdyStateMaxQuantizedBytes = 4096;

	// LEB128 unsigned varint. A u64 is at most 10 continuation groups (7 bits each), so a stream that
	// keeps the high bit set past the 10th byte is malformed and must be rejected rather than shifted
	// past the width of the value.
	constexpr int32 CrowdyStateMaxVarUIntBytes = 10;

	// How many layout slots the encoder's per-emit scratch holds without touching the heap. Sized well
	// above the replicated classes in this project (the widest test fixture is 14) rather than tuned to
	// them, because a class past it merely spills to the heap, which is what every emit did before.
	constexpr int32 CrowdyStateInlineSlots = 64;

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
	// MUST gate on this identically. FVector and FRotator each declare a native net serializer, so both ride
	// this branch and quantize; a plain USTRUCT declaring none rides SerializeItem and round-trips exactly.
	bool IsNetQuantizedProperty(const FProperty* Prop)
	{
		const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		return StructProp != nullptr
			&& StructProp->Struct != nullptr
			&& (StructProp->Struct->StructFlags & STRUCT_NetSerializeNative) != 0;
	}

	/**
	 * The numeric property carrying an enum leaf's raw value, or null when the property is not an enum.
	 *
	 * An enum reaches the wire as its VALUE here, never as its entry name, and that is a deliberate
	 * departure from what FProperty::SerializeItem does for these two declarations. Both
	 * `enum class E : uint8` (an FEnumProperty) and TEnumAsByte (an FByteProperty carrying an enum) are
	 * serialized BY NAME by the engine, and a memory archive writes an FName as a length-prefixed string:
	 * measured, one such field costs 51 bytes against 4 for the plain byte beside it. On a plane where a
	 * field can carry CrowdyHeartbeat, that is paid per entity per keyframe interval, and receive drain is
	 * what bounds the population.
	 *
	 * The name form buys resilience to an enum's values being renumbered, which is worth nothing here: the
	 * layout hash both peers agree on is computed from the same build's reflection, so the two ends always
	 * share a value space. It costs something real in exchange, and it fails badly: an entry RENAMED
	 * between builds leaves the layout hash identical, so the delta is accepted, the name lookup misses,
	 * and the value silently becomes the enum's maximum. A value cannot miss.
	 *
	 * The two declarations need two different treatments, and it is not a stylistic split. An FEnumProperty
	 * OWNS a separate underlying property, whose SerializeItem is the plain integer one, so handing the
	 * value to it is enough. A TEnumAsByte is a single FByteProperty that carries the enum on itself: there
	 * is no second property to defer to, and calling its own SerializeItem would take the very by-name
	 * branch this exists to avoid. That one is written as a raw byte instead, which is byte-identical to
	 * what a plain uint8 leaf already produces.
	 */
	const FNumericProperty* EnumUnderlyingProperty(const FProperty* Prop)
	{
		const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop);
		return EnumProp ? EnumProp->GetUnderlyingProperty() : nullptr;
	}

	const FByteProperty* EnumAsByteProperty(const FProperty* Prop)
	{
		const FByteProperty* ByteProp = CastField<FByteProperty>(Prop);
		return (ByteProp && ByteProp->Enum) ? ByteProp : nullptr;
	}

	// Serializes one value into Writer at its current position. A net-quantized struct is framed as a
	// length prefix plus its NetSerializeItem bytes so the bit-packed sub-encoding never disturbs the
	// main archive's byte position; an enum rides its raw value (see EnumUnderlyingProperty); everything
	// else rides SerializeItem, byte-identical to the RPC plane.
	void EncodeValue(const FProperty* Prop, void* ValuePtr, FArchive& Writer)
	{
		if (const FNumericProperty* Underlying = EnumUnderlyingProperty(Prop))
		{
			// The underlying property is addressed at the VALUE pointer, not through the container: it is
			// owned by the enum property rather than by the struct, so its own offset is not the member's.
			// This is the same branch FEnumProperty::SerializeItem itself falls through to when an archive
			// is neither loading nor saving, taken deliberately rather than by accident.
			FStructuredArchiveFromArchive Adapter(Writer);
			Underlying->SerializeItem(Adapter.GetSlot(), ValuePtr, nullptr);
			return;
		}

		if (const FByteProperty* EnumAsByte = EnumAsByteProperty(Prop))
		{
			uint8 Value = EnumAsByte->GetPropertyValue(ValuePtr);
			Writer << Value;
			return;
		}

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
		// The mirror of EncodeValue's first branch, and it must stay the first branch here too: the two
		// decide the framing of the bytes, so a value written by one and read by the other is the whole
		// contract. A short read leaves Reader in error and drops the delta, exactly as every other value
		// here does. No range check is needed or wanted: every bit pattern the underlying integer can hold
		// is a value the enum's own space either names or does not, and one it does not name is answered
		// by the consumer rather than rejected here.
		if (const FNumericProperty* Underlying = EnumUnderlyingProperty(Prop))
		{
			FStructuredArchiveFromArchive Adapter(Reader);
			Underlying->SerializeItem(Adapter.GetSlot(), ValuePtr, nullptr);
			return !Reader.IsError();
		}

		if (const FByteProperty* EnumAsByte = EnumAsByteProperty(Prop))
		{
			uint8 Value = 0;
			Reader << Value;
			if (Reader.IsError())
			{
				return false;
			}

			EnumAsByte->SetPropertyValue(ValuePtr, Value);
			return true;
		}

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

			// Read where the bytes already are. The bounds above have proven the sub-blob lies inside Blob, so a
			// view over those bytes replaces copying them out into a second array first. ArMaxSerializeSize is
			// pinned to the sub-length so a forged length prefix inside the sub-blob cannot reach past it
			// either. The view carries its own position, so the outer reader is stepped over the sub-blob
			// explicitly once the value is read.
			const int64 SubStart = Reader.Tell();
			FMemoryReaderView SubR(MakeMemoryView(Blob.GetData() + SubStart, static_cast<uint64>(Len)),
				/*bIsPersistent=*/true);
			SubR.ArMaxSerializeSize = Len;
			Prop->NetSerializeItem(SubR, /*Map*/nullptr, ValuePtr);
			if (SubR.IsError())
			{
				UE_LOG(LogCrowdyReplication, Warning,
					TEXT("CrowdyStateCodec::Decode: quantized value for '%s' failed to net-deserialize; dropping."),
					*Prop->GetName());
				return false;
			}

			Reader.Seek(SubStart + Len);
			return true;
		}

		FStructuredArchiveFromArchive Adapter(Reader);
		Prop->SerializeItem(Adapter.GetSlot(), ValuePtr, nullptr);
		return !Reader.IsError();
	}
}

FCrowdyStateDecodeScratch::~FCrowdyStateDecodeScratch()
{
	Release();
}

void FCrowdyStateDecodeScratch::Release()
{
	// Values are destroyed through the properties snapshotted when the block was built, never through a layout
	// that may since have been freed and rebuilt. A block whose class has gone away is ABANDONED instead: its
	// properties belong to that class, so a reload that reinstanced it leaves them dangling and destroying
	// through them would dereference freed memory. Leaking the values of a class that no longer exists is
	// bounded and editor-only.
	if (Block && OwnerClass.IsValid())
	{
		for (int32 Index = 0; Index < SlotProperties.Num(); ++Index)
		{
			if (SlotProperties[Index] && SlotOffsets[Index] != INDEX_NONE)
			{
				SlotProperties[Index]->DestroyValue(Block + SlotOffsets[Index]);
			}
		}
	}

	if (Block)
	{
		FMemory::Free(Block);
		Block = nullptr;
	}

	BlockSize = 0;
	LayoutHash = 0;
	OwnerClass = nullptr;
	SlotProperties.Reset();
	SlotOffsets.Reset();
}

void FCrowdyStateDecodeScratch::EnsureForLayout(const FCrowdyRepLayout& Layout, const UClass* Owner)
{
	// The block is kept only while it was built for this exact live class: a class that was reinstanced or
	// collected leaves the snapshotted properties describing nothing, and the weak handle is what sees that.
	if (Block && Owner && OwnerClass.Get() == Owner && LayoutHash == Layout.LayoutHash)
	{
		return;
	}

	Release();

	if (!Owner)
	{
		return;
	}

	// Pack the slots in layout order, each at its own value's alignment. Sized from the layout rather than from
	// the class, so the block stays small for a layout on a wide class and is never shaped like an instance.
	const int32 N = Layout.Properties.Num();
	SlotProperties.SetNumZeroed(N);
	SlotOffsets.Init(INDEX_NONE, N);

	int32 Size = 0;
	int32 MaxAlignment = 1;
	for (int32 Index = 0; Index < N; ++Index)
	{
		const FProperty* Prop = Layout.Properties[Index].Property;
		if (!Prop)
		{
			continue;
		}

		const int32 Alignment = FMath::Max(1, Prop->GetMinAlignment());
		Size = Align(Size, Alignment);
		SlotProperties[Index] = Prop;
		SlotOffsets[Index] = Size;
		// Static arrays (ArrayDim > 1) are rejected at discovery, so GetSize() is one value's worth.
		Size += Prop->GetSize();
		MaxAlignment = FMath::Max(MaxAlignment, Alignment);
	}

	if (Size <= 0)
	{
		Release();
		return;
	}

	// Zeroed before anything is constructed, so the padding between slots is deterministic.
	Block = static_cast<uint8*>(FMemory::Malloc(Size, MaxAlignment));
	FMemory::Memzero(Block, Size);
	BlockSize = Size;

	for (int32 Index = 0; Index < N; ++Index)
	{
		if (SlotProperties[Index])
		{
			SlotProperties[Index]->InitializeValue(Block + SlotOffsets[Index]);
		}
	}

	OwnerClass = Owner;
	LayoutHash = Layout.LayoutHash;
}

bool FCrowdyStateDecodeScratch::DescribesLayout(const FCrowdyRepLayout& Layout) const
{
	return Block != nullptr && LayoutHash == Layout.LayoutHash && SlotProperties.Num() == Layout.Properties.Num();
}

void* FCrowdyStateDecodeScratch::SlotFor(int32 LayoutIndex) const
{
	if (!Block || !SlotOffsets.IsValidIndex(LayoutIndex) || SlotOffsets[LayoutIndex] == INDEX_NONE)
	{
		return nullptr;
	}
	return Block + SlotOffsets[LayoutIndex];
}

bool FCrowdyStateDecodeScratch::Contains(const void* Address) const
{
	const uint8* const Byte = static_cast<const uint8*>(Address);
	return Block && Byte >= Block && Byte < Block + BlockSize;
}

void FCrowdyStateCodec::Encode(const FCrowdyRepLayout& Layout, const void* Container, const TBitArray<>& Dirty,
	bool bKeyframe, TArray<uint8>& OutBlob)
{
	OutBlob.Reset();

	const int32 N = Layout.Properties.Num();

	// Collect the present layout indices in ascending order. A keyframe forces every slot present; a hot
	// delta takes exactly the set Dirty bits (bounded by N, so an over-long Dirty array cannot leak a
	// slot that does not exist in the layout).
	// Inline storage: this runs once per emitted delta, and a replicated class with more slots than this
	// spills to the heap exactly as it did before rather than being refused.
	TArray<int32, TInlineAllocator<CrowdyStateInlineSlots>> Present;
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
		TArray<uint8, TInlineAllocator<(CrowdyStateInlineSlots + 7) / 8>> Mask;
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
	void* Container, TArray<int32>& OutChangedIndices, TArray<int32>* OutPresentIndices,
	FCrowdyStateDecodeScratch* Scratch)
{
	// Off unless crowdy.state.scopes is set. It nests inside the enclosing delivery scope, so leaving it on
	// would add two timestamps per delta there and make two runs that carry it differently incomparable.
	TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(Crowdy_DecodeStateDelta, CrowdyReplicationProfile::StateScopes());

	OutChangedIndices.Reset();
	if (OutPresentIndices)
	{
		OutPresentIndices->Reset();
	}

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
	// Inline storage, mirroring the encoder: this runs once per received delta, and a replicated class with
	// more slots than this spills to the heap exactly as it did before rather than being refused.
	TArray<int32, TInlineAllocator<CrowdyStateInlineSlots>> Present;
	if (SelectorMode == 0)
	{
		const int32 BitmaskBytes = (N + 7) / 8;
		if (static_cast<int64>(Blob.Num()) - Reader.Tell() < BitmaskBytes)
		{
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("CrowdyStateCodec::Decode: blob too short for a %d-byte bitmask; dropping delta."), BitmaskBytes);
			return false;
		}
		TArray<uint8, TInlineAllocator<(CrowdyStateInlineSlots + 7) / 8>> Mask;
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

	// Reported before the body runs, so it describes the DELTA rather than the outcome of applying it: a caller
	// decoding into a buffer shared between targets needs to know what arrived even for slots whose value
	// happened to match what the previous target left behind. A mid-body failure still returns false and such a
	// caller applies nothing, so publishing this early cannot make a dropped delta look delivered.
	if (OutPresentIndices)
	{
		*OutPresentIndices = Present;
	}

	// A scratch block that lies inside the container would compare every value against itself and report
	// nothing as changed, so it is refused here and each value falls back to its own allocation. The two are
	// separate allocations by construction (a block is sized by the layout, never shaped like a class), so
	// this is a guard against a caller handing over the wrong buffer rather than a case that can arise on its
	// own.
	// A block the caller already built for this layout is taken as it is, so a caller that prepared it does not
	// pay the class handle resolve a second time on every delta.
	if (Scratch && !Scratch->DescribesLayout(Layout))
	{
		Scratch->EnsureForLayout(Layout, Layout.OwnerClass.Get());
	}
	const bool bUseScratch = Scratch && Scratch->IsReady() && !Scratch->Contains(Container);

	// Positional body: decode each present value in ascending index order into a scratch value, then write it
	// onto the live container and record the slot ONLY when it actually differs from the value already there.
	// "Changed" must mean "the value moved", not merely "present in the delta": the keyframe heartbeat
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

		// The scratch value the incoming bytes decode into, so the live value is only touched on a real
		// change. It is the caller's persistent slot for this layout position when one was supplied, and
		// otherwise a value allocated and destroyed here. Static arrays (ArrayDim > 1) are rejected at
		// discovery, so GetSize() is one value's worth.
		void* SlotPtr = bUseScratch ? Scratch->SlotFor(Index) : nullptr;
		const bool bTemporarySlot = (SlotPtr == nullptr);
		if (bTemporarySlot)
		{
			SlotPtr = FMemory::Malloc(Prop->GetSize(), Prop->GetMinAlignment());
			Prop->InitializeValue(SlotPtr);
		}
		else
		{
			// A persistent slot still holds the previous delta's value, so it is returned to the property's
			// default first. A value a decoder writes only in part is then compared against the default,
			// exactly as a freshly constructed one always was, rather than against what the last delta left.
			Prop->DestroyValue(SlotPtr);
			Prop->InitializeValue(SlotPtr);
		}

		const bool bDecoded = DecodeValue(Prop, SlotPtr, Reader, Blob);
		if (bDecoded)
		{
			void* LivePtr = Prop->ContainerPtrToValuePtr<void>(Container);
			if (!Prop->Identical(SlotPtr, LivePtr, PPF_None))
			{
				Prop->CopyCompleteValue(LivePtr, SlotPtr);
				OutChangedIndices.Add(Index);
			}
		}

		// A persistent slot is left constructed and is destroyed with the block, which keeps exactly one live
		// value per slot at every point, including the failure return below.
		if (bTemporarySlot)
		{
			Prop->DestroyValue(SlotPtr);
			FMemory::Free(SlotPtr);
		}

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

	// Bounded reader (caps ArMaxSerializeSize so any FString/FName length prefix is
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
