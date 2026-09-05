#include "Utils/SerializationFunctionLibrary.h"
#include "CrowdyNetLog.h"
#include <atomic>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Utils/CrowdyPodCopyPlan.h"
#include "Utils/UActorUpdatePayloadRegistry.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/Archive.h"
#include "Utils/UEventPayloadRegistry.h"

FString USerializationFunctionLibrary::DeserializeString(const TConstArrayView<uint8> Payload, int32 Offset, int32 Length)
{
	if (Offset + Length > Payload.Num())
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("Offset + Length exceeds Payload size"));
		return FString();
	}

	// Convert directly from the UTF-8 byte range with explicit length
	const char* Utf8Ptr = reinterpret_cast<const char*>(Payload.GetData() + Offset);
	const FUTF8ToTCHAR Converter(Utf8Ptr, Length);

	// Build an FString from the converted buffer
	return FString(Converter.Length(), Converter.Get());
}

int32 USerializationFunctionLibrary::DeserializeInt32(const TArray<uint8>& Payload, int32 Offset)
{
	int32 Value = 0;
	if (Offset + 4 <= Payload.Num())
	{
		FMemory::Memcpy(&Value, Payload.GetData() + Offset, 4);
	}
	return Value;
}

int64 USerializationFunctionLibrary::DeserializeInt64(const TArray<uint8>& Payload, int32 Offset)
{
	int64 Value = 0;
	if (Offset + sizeof(int64) <= Payload.Num())
	{
		FMemory::Memcpy(&Value, Payload.GetData() + Offset, sizeof(int64));
	}
	return Value;
}

float USerializationFunctionLibrary::DeserializeFloat(const TArray<uint8>& Payload, int32 Offset)
{
	if (Payload.Num() < Offset + sizeof(float))
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("Buffer too small to deserialize float."));
		return 0.0f;
	}

	// Directly copy the bytes from the payload to the float
	float Value;
	FMemory::Memcpy(&Value, &Payload[Offset], sizeof(float));
	return Value;
}

TArray<uint8> USerializationFunctionLibrary::CalculateHMAC(const TArray<uint8>& Payload, const FString& GameToken)
{
	TArray<uint8> HMACResult;

	const FTCHARToUTF8 Converter(*GameToken);
	const uint8* TokenBytes = reinterpret_cast<const uint8*>(Converter.Get());
	const int32 TokenLen = Converter.Length();

	// Ensure key is exactly 64 bytes
	check(TokenLen == 64);

	TArray<uint8> Key;
	Key.Append(TokenBytes, TokenLen);

	TArray<uint8> Message;
	Message.Reserve(Payload.Num() + Key.Num());
	Message.Append(Payload);
	Message.Append(Key);

	uint8 HMACBuffer[32] = {0};
	unsigned int OutLen = 0;

	HMAC(
		EVP_sha256(),
		Key.GetData(),
		Key.Num(),
		Message.GetData(),
		Message.Num(),
		HMACBuffer,
		&OutLen
	);

	HMACResult.Append(HMACBuffer, OutLen);

	return HMACResult;
}

bool USerializationFunctionLibrary::AuthenticateHMAC(const TArray<uint8>& ReceivedMessage, const FString& GameToken)
{
	constexpr int32 HmacSize = 32;
	constexpr int32 TailSizeWithAuth = 41;

	if (ReceivedMessage.IsEmpty())
	{
		return false;
	}

	// Non-spatial messages (type high bit clear, e.g. channel notifications and bundles) are not
	// HMAC-signed server->client, so there is nothing to verify. Decide on the type byte directly
	// rather than the byte[35] containsAuth flag, which only lines up for the spatial header.
	if ((ReceivedMessage[0] & 0x80) == 0)
	{
		return true;
	}

	if (ReceivedMessage.Num() < TailSizeWithAuth)
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("[SerializationFunctionLibrary]: Received message too short to authenticate"));
		return false;
	}

	const bool bContainsAuth = ReceivedMessage[35] == 1;

	if (!bContainsAuth)
	{
		//UE_LOG(LogCrowdyNet, Error, TEXT("[SerializationFunctionLibrary]: Received message does not contain HMAC. But is allowed."));
		return true;
	}

	const int32 PrefixLen = ReceivedMessage.Num() - TailSizeWithAuth;

	if (PrefixLen <= 0)
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("[SerializationFunctionLibrary]: Received message too short to authenticate"));
		return false;
	}

	const uint8* MessageData = ReceivedMessage.GetData();
	const uint8* ReceivedHmac = MessageData + PrefixLen;

	const TArray Prefix(MessageData, PrefixLen);
	TArray<uint8> ComputedHmac = CalculateHMAC(Prefix, GameToken);

	if (ComputedHmac.Num() != HmacSize)
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("[SerializationFunctionLibrary]: HMAC calculation failed"));
		return false;
	}

	return CRYPTO_memcmp(ComputedHmac.GetData(), ReceivedHmac, HmacSize) == 0;
}

bool USerializationFunctionLibrary::ExtractChunkCoordinates(const TSharedPtr<FJsonObject>& JsonObj, int64& X, int64& Y,
                                                            int64& Z)
{
	const TSharedPtr<FJsonObject>* Coordinates;

	if (!JsonObj->TryGetObjectField(TEXT("coordinates"), Coordinates))
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("Coordinates not found in updateChunk object"));
		return false;
	}

	FString sX, sY, sZ;
	if (!(*Coordinates)->TryGetStringField(TEXT("x"), sX))
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("X coord not found in coordinates object or failed to extract coord"));
		return false;
	}

	if (!(*Coordinates)->TryGetStringField(TEXT("y"), sY))
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("Y coord not found in coordinates object or failed to extract coord"));
		return false;
	}

	if (!(*Coordinates)->TryGetStringField(TEXT("z"), sZ))
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("Z coord not found in coordinates object or failed to extract coord"));
		return false;
	}

	X = FCString::Atoi64(*sX);
	Y = FCString::Atoi64(*sY);
	Z = FCString::Atoi64(*sZ);

	return true;
}

FGuid USerializationFunctionLibrary::ToGuid(const FCrowdyActorId& ActorId)
{
	// Runs once per received actor update, so it stays scoped and stays branch-free over the 32 octets.
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_ToGuid);

	// An id that was never filled in addresses no actor, so it derives no key. Without this it would
	// derive the same key every id outside the hexadecimal alphabet derives, and that key reports itself
	// as valid, so "no actor" would be indistinguishable from an id a sender chose.
	if (!ActorId.IsSet())
	{
		return FGuid();
	}

	// Classified once at compile time, so an octet costs one load rather than three range compares.
	// Declared in the function so no other translation unit can collide with it under a unity build.
	struct FHexDigitTable
	{
		enum : uint8 { NotAHexDigit = 255 };

		uint8 Values[256];

		constexpr FHexDigitTable() : Values()
		{
			for (int32 Index = 0; Index < 256; ++Index) Values[Index] = FHexDigitTable::NotAHexDigit;
			for (int32 Index = '0'; Index <= '9'; ++Index) Values[Index] = static_cast<uint8>(Index - '0');
			for (int32 Index = 'A'; Index <= 'F'; ++Index) Values[Index] = static_cast<uint8>(Index - 'A' + 10);
			for (int32 Index = 'a'; Index <= 'f'; ++Index) Values[Index] = static_cast<uint8>(Index - 'a' + 10);
		}
	};

	static constexpr FHexDigitTable HexDigits{};

	// A component holding anything but hexadecimal reads as the largest value, so two ids that differ
	// only outside the hexadecimal alphabet derive the same key. The 32 octets remain what tells them
	// apart; this is a local lookup key and nothing more. A component is judged after all eight octets
	// rather than at the first bad one, which reads the same because every rejection is the same value.
	auto ParseHex = [](const uint8* Octets, const int32 Len) -> uint32
	{
		uint32 Result = 0;
		uint32 Seen = 0;

		for (int32 i = 0; i < Len; ++i)
		{
			const uint8 Value = HexDigits.Values[Octets[i]];
			Seen |= Value;
			Result = (Result << 4) | (Value & 0xFu);
		}

		return Seen >= FHexDigitTable::NotAHexDigit ? UINT32_MAX : Result;
	};

	const uint32 A = ParseHex(ActorId.Octets, 8);
	const uint32 B = ParseHex(ActorId.Octets + 8, 8);
	const uint32 C = ParseHex(ActorId.Octets + 16, 8);
	const uint32 D = ParseHex(ActorId.Octets + 24, 8);

	return FGuid(A, B, C, D);
}

FGuid USerializationFunctionLibrary::ToGuid(const FString& String)
{
	// Text that is not exactly the 32 octets an actor id occupies describes no actor, so it derives no
	// key. Reading a fixed 32 characters out of a shorter string would read past its end.
	FCrowdyActorId ActorId;
	if (!FCrowdyActorId::TryFromString(String, ActorId))
	{
		return FGuid();
	}

	return ToGuid(ActorId);
}

FString USerializationFunctionLibrary::GenerateVoxelID(int64 ChunkX, int64 ChunkY, int64 ChunkZ, int32 VoxelX,
                                                       int32 VoxelY, int32 VoxelZ)
{
	const FString Input = FString::Printf(
		TEXT("%lld,%lld,%lld,%d,%d,%d"),
		ChunkX, ChunkY, ChunkZ,
		VoxelX, VoxelY, VoxelZ);

	// Convert to UTF-8 for hashing
	const FTCHARToUTF8 UTF8String(*Input);

	// Calculate SHA256 hash using OpenSSL
	unsigned char HashBytes[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char*>(UTF8String.Get()), UTF8String.Length(), HashBytes);

	// Convert hash to hex string
	FString Result;
	for (int32 i = 0; i < SHA256_DIGEST_LENGTH; ++i)
	{
		Result += FString::Printf(TEXT("%02x"), HashBytes[i]);
	}

	return Result;
}

namespace
{
	// A peer can emit malformed payloads as fast as it can send them, and on an open network some peer
	// eventually will, so one log line per bad payload would let a sender fill a player's log. At most one
	// line is emitted per interval, and it carries how many went unreported, so a flood still shows up as
	// a count instead of as pages of text.
	constexpr double WirePayloadReportIntervalSeconds = 5.0;

	std::atomic<double> LastWirePayloadReportSeconds{ 0.0 };
	std::atomic<int32> WirePayloadFaultsSinceLastReport{ 0 };

	bool ShouldReportWirePayloadFault(int32& OutSuppressedSinceLastReport)
	{
		const double Now = FPlatformTime::Seconds();
		double LastReport = LastWirePayloadReportSeconds.load(std::memory_order_relaxed);

		// Payloads are decoded on the receive threads as well as on the game thread. Losing the exchange
		// means another thread has just claimed this interval's line, which counts the same as arriving
		// inside the interval.
		if (Now - LastReport < WirePayloadReportIntervalSeconds
			|| !LastWirePayloadReportSeconds.compare_exchange_strong(LastReport, Now, std::memory_order_relaxed))
		{
			WirePayloadFaultsSinceLastReport.fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		OutSuppressedSinceLastReport = WirePayloadFaultsSinceLastReport.exchange(0, std::memory_order_relaxed);
		return true;
	}

	// Warning rather than Error: a truncated, forged or out-of-date packet is an ordinary event on an open
	// network, not a fault in this build, and it must not read as one.
	void ReportWirePayloadFault(const TCHAR* Context, const TCHAR* Detail, const uint32 TypeID, const int32 PayloadBytes)
	{
		int32 SuppressedSinceLastReport = 0;

		if (!ShouldReportWirePayloadFault(SuppressedSinceLastReport))
		{
			return;
		}

		UE_LOG(LogCrowdyNet, Warning,
			TEXT("[%s]: %s (TypeID=%u, %d payload bytes). Similar reports suppressed since the last one: %d."),
			Context, Detail, TypeID, PayloadBytes, SuppressedSinceLastReport);
	}

	// Caps the length any one string or object path inside the payload may claim for itself. A string of N
	// characters occupies at least N bytes on the wire, so nothing well formed can ever claim more
	// characters than the payload carrying it has bytes: the payload's own size is therefore a ceiling
	// that rejects no valid frame, and it tracks whatever limit the transport already enforces instead of
	// restating a number of its own. Without it a forged length prefix is a request to reserve that much
	// memory before a single byte of the string is read.
	void BoundReaderToPayload(FArchive& Reader, const TConstArrayView<uint8> Payload)
	{
		Reader.ArMaxSerializeSize = Payload.Num();
	}

	// Payload structs are grown by appending fields, and the wire type tag hashes the struct's path name
	// only, so the tag does not change when one grows. A peer a version behind therefore writes fewer
	// bytes than this build reads, and a peer a version ahead writes more. Only the second case can be
	// honoured: every field this build knows about was read in full and the bytes left over describe
	// fields it has no names for, so the decode stands and the tail is dropped.
	//
	// Refusing every tail would break that skew on purpose; accepting one without a word would hide both a
	// version drift and a sender padding whatever it likes onto an otherwise valid frame. So a tail is
	// always accounted for, and once it is larger than everything that was understood it leaves the trace
	// channel and is reported: past that point the two ends disagree about this type more than they agree,
	// and the fields that did decode are worth doubting. The frame is still accepted, because the bytes
	// were already received and the fields ahead of the tail are complete.
	void ReportUnreadTail(const TCHAR* Context, const uint32 TypeID, const int64 ConsumedBytes, const int32 PayloadBytes)
	{
		const int64 UnreadBytes = PayloadBytes - ConsumedBytes;

		if (UnreadBytes <= 0)
		{
			return;
		}

		if (UnreadBytes > ConsumedBytes)
		{
			ReportWirePayloadFault(Context,
				TEXT("more bytes were left unread than were decoded, so the sender's idea of this type is not this build's"),
				TypeID, PayloadBytes);
			return;
		}

		UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log,
			TEXT("[%s]: TypeID=%u decoded, %d of %d bytes left unread, which is how a peer a version ahead reads."),
			Context, TypeID, static_cast<int32>(UnreadBytes), PayloadBytes);
	}

	// What a payload that ended before this build's version of the struct did should mean. The two
	// framings answer this differently, and the difference is the whole reason it is a parameter.
	enum class EShortPayloadPolicy : uint8
	{
		// The fields that were not reached keep the values the struct constructs them with, and those
		// defaults are chosen to be safe. Used for actor state, where a struct only ever grows by
		// appending and the tag hashes the path name alone, so a peer one version behind writes a
		// genuinely valid frame that is simply shorter. Rejecting it would drop every update from that
		// peer, which is a worse failure than reading its newest fields as their defaults.
		FieldsKeepTheirDefaults,

		// Nothing is handed on. Used for events, whose payloads are call arguments rather than a view
		// of a world: an argument nobody sent is not the same as an argument that happens to equal its
		// default, and invoking a handler with made-up arguments is worse than not invoking it.
		Reject
	};

	// The verdict on a decode that reached the end of its framing. A short read leaves the reader
	// flagged and every field it could not reach at that field's default; whether that is a usable
	// frame or a discarded one is the caller's contract, not this function's.
	bool FinishPayloadDecode(const TCHAR* Context, FArchive& Reader, const TConstArrayView<uint8> Payload,
		FInstancedStruct& OutPayload, const uint32 TypeID, const EShortPayloadPolicy ShortPayloadPolicy)
	{
		if (Reader.IsError())
		{
			if (ShortPayloadPolicy == EShortPayloadPolicy::Reject)
			{
				OutPayload.Reset();
				ReportWirePayloadFault(Context, TEXT("the payload ended before the struct it declares did"), TypeID,
					Payload.Num());
				return false;
			}

			// Accepted, but never silently: a peer whose idea of this type is shorter than ours is the
			// one thing this framing cannot see for itself, and the fields left at their defaults are
			// indistinguishable from fields that really carried them. Reporting it is what turns a
			// version skew from a mystery into a line in the log.
			ReportWirePayloadFault(Context,
				TEXT("the payload ended before the struct it declares did, so the fields past its end keep their defaults, which is how a peer a version behind reads"),
				TypeID, Payload.Num());
			return true;
		}

		ReportUnreadTail(Context, TypeID, Reader.Tell(), Payload.Num());
		return true;
	}

	// A payload struct's body, moved by the baked copy plan for a struct whose bytes were proven
	// identical either way, and by the reflective property walk for every other struct. The direction
	// comes from the archive, so the two ends of a write and its matching read cannot take different
	// halves of this decision.
	void SerializeStructBody(const UScriptStruct* StructType, FArchive& Ar, void* StructMemory)
	{
		const FCrowdyCopyPlan* Plan = CrowdyPodCopyPlan::Find(StructType);

		if (Plan && CrowdyPodCopyPlan::Apply(*Plan, Ar, StructMemory))
		{
			return;
		}

		StructType->SerializeBin(Ar, StructMemory);
	}
}

bool USerializationFunctionLibrary::SerializeActorState(const FInstancedStruct& Payload, const FCrowdyClassID ClassID,
	TArray<uint8>& OutBytes)
{
	const UScriptStruct* StructType = Payload.GetScriptStruct();
	const void* StructMemory = Payload.GetMemory();

	if (!StructType || !StructMemory)
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("[SerializeActorState]: Invalid script struct or memory pointer"));
		return false;
	}

	FCrowdyTypeID TypeID;

	if (!UActorUpdatePayloadRegistry::Get()->GetID(StructType, TypeID))
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("[SerializeActorState]: Failed to get ID for script struct"));
		return false;
	}

	// A class whose registration lost an ID clash resolves to no ID at all. A spawn event survives that
	// because it also carries the class path, but an update has no room for one, so sending it would
	// leave the receiver deriving the class from the struct instead: two classes sharing a struct would
	// then collapse into whichever registered last, which is the whole defect this framing removes.
	// Refusing here keeps that failure loud and local to the sender.
	if (ClassID == CROWDY_INVALID_CLASS_ID)
	{
		UE_LOG(LogCrowdyNet, Error,
			TEXT("[SerializeActorState]: no class id for a '%s' update, so a receiver could only guess its class from the struct. Refusing to send. A registration refused over an id clash is the usual cause."),
			*StructType->GetName());
		return false;
	}

	OutBytes.Reset();
	FMemoryWriter Writer(OutBytes, true);

	// Named locals because operator<< takes a non-const reference.
	FCrowdyTypeID Sentinel = CROWDY_ACTOR_STATE_SENTINEL;
	uint8 FormatVersion = CROWDY_ACTOR_STATE_FORMAT_VERSION;
	FCrowdyClassID WireClassID = ClassID;

	Writer << Sentinel;
	Writer << FormatVersion;
	Writer << TypeID;
	Writer << WireClassID;

	SerializeStructBody(StructType, Writer, const_cast<void*>(StructMemory));

	//UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log, TEXT("[SerializeActorState] '%s' -> TypeID=%d, Size=%d bytes"),
	//	*StructType->GetName(), TypeID, OutBytes.Num());

	return true;
}

bool USerializationFunctionLibrary::DeserializeActorState(const TConstArrayView<uint8> Payload, FInstancedStruct& OutPayload)
{
	FCrowdyTypeID DiscardedTypeID = CROWDY_INVALID_TYPE_ID;
	return DeserializeActorState(Payload, OutPayload, DiscardedTypeID);
}

bool USerializationFunctionLibrary::DeserializeActorState(const TConstArrayView<uint8> Payload, FInstancedStruct& OutPayload,
	FCrowdyTypeID& OutTypeID)
{
	FCrowdyClassID DiscardedClassID = CROWDY_INVALID_CLASS_ID;
	return DeserializeActorState(Payload, OutPayload, OutTypeID, DiscardedClassID);
}

bool USerializationFunctionLibrary::DeserializeActorState(const TConstArrayView<uint8> Payload, FInstancedStruct& OutPayload,
	FCrowdyTypeID& OutTypeID, FCrowdyClassID& OutClassID)
{
	// The payload-decode share of per-message cost specifically, which is the number that decides whether
	// shrinking the payload can move the receive ceiling at all. Quantizing the transform only helps
	// through the part of delivery that scales with payload size, and that part is this scope.
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_DeserializeActorState);

	OutTypeID = CROWDY_INVALID_TYPE_ID;
	OutClassID = CROWDY_INVALID_CLASS_ID;

	// Sentinel, format version, type tag and class id, all of which are read before any of the body.
	static constexpr int32 FramingBytes =
		sizeof(FCrowdyTypeID) + sizeof(uint8) + sizeof(FCrowdyTypeID) + sizeof(FCrowdyClassID);

	if (Payload.Num() < FramingBytes)
	{
		// Emptied on the way out, like every other rejection here. A caller that reuses one payload
		// across calls would otherwise still be holding the previous update, and a rejected frame that
		// leaves last frame's values in place is worse than one that leaves nothing.
		OutPayload.Reset();
		ReportWirePayloadFault(TEXT("DeserializeActorState"),
			TEXT("the payload is shorter than the framing it has to lead with"), CROWDY_INVALID_TYPE_ID,
			Payload.Num());
		return false;
	}

	FMemoryReaderView Reader(Payload, true);
	BoundReaderToPayload(Reader, Payload);

	// Seeded rather than left indeterminate: on a blob too short to hold the field the reader flags an
	// error and leaves the value untouched.
	FCrowdyTypeID Sentinel = CROWDY_INVALID_TYPE_ID;
	Reader << Sentinel;

	if (Reader.IsError())
	{
		OutPayload.Reset();
		ReportWirePayloadFault(TEXT("DeserializeActorState"), TEXT("the framing sentinel could not be read"),
			CROWDY_INVALID_TYPE_ID, Payload.Num());
		return false;
	}

	// A sender predating this framing leads with its own type tag, and no struct ever hashes to zero,
	// so a non-zero value here names a build that cannot have written a class id. Its bytes are refused
	// rather than read: continuing would take four bytes of a transform as an entity's class, which is
	// precisely the silent misparse the sentinel exists to make impossible.
	if (Sentinel != CROWDY_ACTOR_STATE_SENTINEL)
	{
		OutPayload.Reset();
		ReportWirePayloadFault(TEXT("DeserializeActorState"),
			TEXT("the frame leads with a type tag rather than the framing sentinel, so its sender predates class identity on the wire"),
			Sentinel, Payload.Num());
		return false;
	}

	uint8 FormatVersion = 0;
	Reader << FormatVersion;

	if (Reader.IsError())
	{
		OutPayload.Reset();
		ReportWirePayloadFault(TEXT("DeserializeActorState"), TEXT("the format version could not be read"),
			CROWDY_INVALID_TYPE_ID, Payload.Num());
		return false;
	}

	// The only rejection on skew this path makes, and it happens before the body is touched on purpose.
	// Every decode below tolerates a peer a version apart, because a payload struct grows by appending
	// and a shorter frame from an older peer is still a valid one. That tolerance cannot distinguish an
	// appended field from a reshaped frame, so the reshaping has to be caught here or not at all.
	if (FormatVersion != CROWDY_ACTOR_STATE_FORMAT_VERSION)
	{
		OutPayload.Reset();
		ReportWirePayloadFault(TEXT("DeserializeActorState"),
			TEXT("the frame declares an actor-state format version this build does not decode"),
			FormatVersion, Payload.Num());
		return false;
	}

	FCrowdyTypeID TypeID = CROWDY_INVALID_TYPE_ID;
	Reader << TypeID;

	if (Reader.IsError())
	{
		OutPayload.Reset();
		ReportWirePayloadFault(TEXT("DeserializeActorState"), TEXT("the type tag could not be read"),
			CROWDY_INVALID_TYPE_ID, Payload.Num());
		return false;
	}

	OutTypeID = TypeID;

	FCrowdyClassID ClassID = CROWDY_INVALID_CLASS_ID;
	Reader << ClassID;

	if (Reader.IsError())
	{
		OutPayload.Reset();
		ReportWirePayloadFault(TEXT("DeserializeActorState"), TEXT("the class id could not be read"),
			TypeID, Payload.Num());
		return false;
	}

	// The sender refuses to emit this, so seeing it means a forged or corrupt frame rather than an
	// honest peer. Refusing it keeps the guarantee the framing is built on: an accepted update always
	// names its own class, and no code downstream has to fall back to guessing one from the struct.
	if (ClassID == CROWDY_INVALID_CLASS_ID)
	{
		OutPayload.Reset();
		ReportWirePayloadFault(TEXT("DeserializeActorState"),
			TEXT("the frame carries no class id, which a conforming sender never emits"), TypeID, Payload.Num());
		return false;
	}

	OutClassID = ClassID;

	const UScriptStruct* StructType = UActorUpdatePayloadRegistry::Get()->Resolve(TypeID);

	if (!StructType)
	{
		//UE_LOG(LogCrowdyNet, Error, TEXT("[DeserializeActorState]: Failed to resolve script struct for TypeID=%d"), TypeID);
		OutPayload.Reset();
		return false;
	}

	OutPayload.InitializeAs(StructType);

	// Moving the struct's body alone. InitializeAs above is deliberately outside it: the walk and the
	// allocation are replaced by different work, so one number covering both cannot size either.
	//
	// Off unless crowdy.serialize.scopes is set. This scope is nested inside Crowdy_DeserializeActorState,
	// so leaving it on would add two timestamps per message to the enclosing scope's own figure and make
	// two runs that carry it differently incomparable.
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_CONDITIONAL(Crowdy_ActorStateSerializeBin, CrowdyNetProfile::DecodeScopes());
		SerializeStructBody(StructType, Reader, OutPayload.GetMutableMemory());
	}

	//UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log, TEXT("[DeserializeActorState] TypeID=%d -> '%s'"), TypeID, *StructType->GetName());

	return FinishPayloadDecode(TEXT("DeserializeActorState"), Reader, Payload, OutPayload, TypeID,
		EShortPayloadPolicy::FieldsKeepTheirDefaults);
}

bool USerializationFunctionLibrary::SerializeEventState(const FInstancedStruct& Payload, TArray<uint8>& OutBytes)
{
	const UScriptStruct* StructType = Payload.GetScriptStruct();

	if (!StructType || !Payload.GetMemory())
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("[SerializeEventState]: Invalid script struct or memory pointer"));
		return false;
	}

	FCrowdyTypeID TypeID;
	if (!UEventPayloadRegistry::Get()->GetID(StructType, TypeID))
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("[SerializeEventState]: Failed to get ID for script struct"));
		return false;
	}

	return SerializeEventState(Payload, TypeID, OutBytes);
}

bool USerializationFunctionLibrary::SerializeEventState(const FInstancedStruct& Payload, const FCrowdyTypeID TypeID,
	TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	return AppendEventState(Payload.GetScriptStruct(), Payload.GetMemory(), TypeID, OutBytes);
}

bool USerializationFunctionLibrary::AppendEventState(const UScriptStruct* StructType, const void* StructMemory,
	const FCrowdyTypeID TypeID, TArray<uint8>& OutBytes)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_SerializeEventState);

	if (!StructType || !StructMemory)
	{
		UE_LOG(LogCrowdyNet, Error, TEXT("[AppendEventState]: Invalid script struct or memory pointer"));
		return false;
	}

	// Appends rather than overwrites, so a caller can serialize the payload straight into the frame it is
	// already building instead of into a buffer that then has to be copied in.
	FMemoryWriter Writer(OutBytes, /*bIsPersistent=*/true, /*bSetOffset=*/true);

	FCrowdyTypeID WireTypeID = TypeID;
	Writer << WireTypeID;

	// The spawn and destroy events are written field by field rather than as flat structs, so the
	// choice of framing is made from the payload's own type. Deciding it from the type ID instead
	// would apply this framing to whatever type happens to hold that ID.
	if (StructType == FCrowdyEntitySpawnEvent::StaticStruct())
	{
		const FCrowdyEntitySpawnEvent* Event =
			static_cast<const FCrowdyEntitySpawnEvent*>(StructMemory);

		FGuid ObjectID = Event->EntityID;
		FGuid OwnerID = Event->OwnerID;
		uint32 ClassID = Event->ClassID;
		FString ClassPath = Event->ClassPath;
		FTransform SpawnTransform = Event->SpawnTransform;

		Writer << ObjectID;
		Writer << OwnerID;
		Writer << ClassID;
		Writer << ClassPath;
		Writer << SpawnTransform;

		const bool bHasInitialState = Event->InitialState.IsValid();
		Writer << const_cast<bool&>(bHasInitialState);

		if (!bHasInitialState)
		{
			return true;
		}

		const UScriptStruct* InnerStructType =
			Event->InitialState.GetScriptStruct();

		const void* InnerStructMemory =
			Event->InitialState.GetMemory();

		FCrowdyTypeID InnerTypeID;
		if (!UEventPayloadRegistry::Get()->GetID(
			InnerStructType,
			InnerTypeID))
		{
			// The one line explaining why a spawn event vanished, now that an unencodable payload drops to
			// an empty frame rather than being reported by the caller. It was filed under LogTemp, which
			// is not the category anyone tailing this path is watching.
			UE_LOG(LogCrowdyNet, Error,
				TEXT("[AppendEventState]: Failed to get ID for InitialState struct"));
			return false;
		}

		Writer << InnerTypeID;
		InnerStructType->SerializeBin(
			Writer,
			const_cast<void*>(InnerStructMemory));

		return true;
	}

	if (StructType == FCrowdyEntityDestroyEvent::StaticStruct())
	{
		const FCrowdyEntityDestroyEvent* Event = static_cast<const FCrowdyEntityDestroyEvent*>(StructMemory);
		FGuid ObjectID = Event->EntityID;
		Writer << ObjectID;
		return true;
	}

	// Default: a flat struct, written straight through.
	SerializeStructBody(StructType, Writer, const_cast<void*>(StructMemory));
	return true;
}

bool USerializationFunctionLibrary::DeserializeEventState(const TConstArrayView<uint8> Payload, FInstancedStruct& OutPayload)
{
	// The blob leads with the payload type tag, so anything shorter than that tag names no type and
	// cannot be read at all.
	if (Payload.Num() < sizeof(FCrowdyTypeID))
	{
		ReportWirePayloadFault(TEXT("DeserializeEventState"),
			TEXT("the payload is shorter than the type tag it has to lead with"), CROWDY_INVALID_TYPE_ID,
			Payload.Num());
		return false;
	}

	FMemoryReaderView Reader(Payload, true);
	BoundReaderToPayload(Reader, Payload);

	// Seeded rather than left indeterminate: on a blob too short to hold the tag the reader flags an
	// error and leaves the value untouched.
	FCrowdyTypeID TypeID = CROWDY_INVALID_TYPE_ID;
	Reader << TypeID;

	if (Reader.IsError())
	{
		ReportWirePayloadFault(TEXT("DeserializeEventState"), TEXT("the type tag could not be read"),
			CROWDY_INVALID_TYPE_ID, Payload.Num());
		return false;
	}

	const UScriptStruct* StructType = UEventPayloadRegistry::Get()->Resolve(TypeID);
	if (!StructType)
	{
		ReportWirePayloadFault(TEXT("DeserializeEventState"),
			TEXT("the payload names a type this build does not know"), TypeID, Payload.Num());
		return false;
	}

	OutPayload.InitializeAs(StructType);
	void* StructMemory = OutPayload.GetMutableMemory();

	// The framing follows the type the ID resolved to, never the ID itself. The buffer above is sized
	// for that type, so reading the spawn fields into anything else would write past the end of it.
	if (StructType == FCrowdyEntitySpawnEvent::StaticStruct())
	{
		auto* Event =
			static_cast<FCrowdyEntitySpawnEvent*>(StructMemory);

		Reader << Event->EntityID;
		Reader << Event->OwnerID;
		Reader << Event->ClassID;
		Reader << Event->ClassPath;
		Reader << Event->SpawnTransform;

		bool bHasInitialState = false;
		Reader << bHasInitialState;

		if (!bHasInitialState)
		{
			Event->InitialState.Reset();
			return FinishPayloadDecode(TEXT("DeserializeEventState"), Reader, Payload, OutPayload, TypeID,
				EShortPayloadPolicy::Reject);
		}

		FCrowdyTypeID InnerTypeID = CROWDY_INVALID_TYPE_ID;
		Reader << InnerTypeID;

		const UScriptStruct* InnerType =
			UEventPayloadRegistry::Get()->Resolve(InnerTypeID);

		if (!InnerType)
		{
			ReportWirePayloadFault(TEXT("DeserializeEventState"),
				TEXT("the spawn event's initial state names a type this build does not know"), InnerTypeID,
				Payload.Num());

			// The spawn fields around it decoded, but the state the event exists to carry did not, so
			// the event is dropped whole rather than delivered as a spawn with nothing in it.
			OutPayload.Reset();
			return false;
		}

		Event->InitialState.InitializeAs(InnerType);

		InnerType->SerializeBin(
			Reader,
			Event->InitialState.GetMutableMemory());

		return FinishPayloadDecode(TEXT("DeserializeEventState"), Reader, Payload, OutPayload, TypeID,
			EShortPayloadPolicy::Reject);
	}

	if (StructType == FCrowdyEntityDestroyEvent::StaticStruct())
	{
		auto* Event = static_cast<FCrowdyEntityDestroyEvent*>(StructMemory);
		Reader << Event->EntityID;
		return FinishPayloadDecode(TEXT("DeserializeEventState"), Reader, Payload, OutPayload, TypeID,
			EShortPayloadPolicy::Reject);
	}

	// Default: a flat struct, read straight through.
	SerializeStructBody(StructType, Reader, StructMemory);
	return FinishPayloadDecode(TEXT("DeserializeEventState"), Reader, Payload, OutPayload, TypeID,
		EShortPayloadPolicy::Reject);
}


void USerializationFunctionLibrary::LogStructContent(const FInstancedStruct& Payload)
{
	const UScriptStruct* StructType = Payload.GetScriptStruct();
	const void* StructMemory = Payload.GetMemory();

	if (!StructType || !StructMemory)
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("[LogStructContents] Empty or invalid FInstancedStruct."));
		return;
	}

	UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log, TEXT("[LogStructContents] Struct: %s"), *StructType->GetName());

	for (TFieldIterator<FProperty> It(StructType); It; ++It)
	{
		FProperty* Prop = *It;
		const void* PropMemory = Prop->ContainerPtrToValuePtr<void>(StructMemory);

		FString ValueStr;
		Prop->ExportTextItem_Direct(ValueStr, PropMemory, nullptr, nullptr, PPF_None);

		UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log, TEXT("  %s = %s"), *Prop->GetName(), *ValueStr);
	}
}
