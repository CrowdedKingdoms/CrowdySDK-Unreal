#pragma once
#include "CoreMinimal.h"
#include "Containers/StringConv.h"
#include "Hash/CityHash.h"
#include "Misc/Crc.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/CrowdyKitEvents.h"

/**
 * Derives the wire identifiers Crowdy puts in front of a payload.
 *
 * Every ID in this file is hashed from the UTF-8 bytes of a name, never from
 * the raw TCHAR bytes. The ID travels on the wire, so its value has to be
 * reproducible from the name alone by any client, without depending on how
 * that client's platform or SDK happens to hold a string in memory. Route any
 * new ID through one of the two hashing primitives below so the rule cannot
 * drift apart again.
 */
struct FCrowdyTypeIDGenerator
{
	// 32-bit hash of a name over its UTF-8 bytes.
	static uint32 HashNameUtf8(const FString& Name)
	{
		const FTCHARToUTF8 Utf8(*Name);
		return FCrc::MemCrc32(Utf8.Get(), Utf8.Length());
	}

	// 64-bit hash of a name over its UTF-8 bytes.
	static uint64 HashNameUtf8_64(const FString& Name)
	{
		const FTCHARToUTF8 Utf8(*Name);
		return CityHash64(reinterpret_cast<const char*>(Utf8.Get()), Utf8.Length());
	}

	// A number the platform fixes for its own events, which no struct may take.
	static constexpr bool IsReservedTypeID(const uint32 TypeID)
	{
		return TypeID == CrowdyKitEvent::Emote;
	}

	// Maps a hash into 1..65535. Persistence and actor state key on this number, so it never changes.
	static constexpr FCrowdyTypeID TypeIDFromHash(const uint32 Hash)
	{
		return static_cast<FCrowdyTypeID>((Hash % 65535u) + 1u);
	}

	// The same mapping for an event payload, which also avoids a reserved number: a hash that would land on one is
	// read again from its high part, so every peer settles on the same number and every other hash is unchanged.
	static constexpr FCrowdyTypeID EventTypeIDFromHash(const uint32 Hash)
	{
		const FCrowdyTypeID First = TypeIDFromHash(Hash);
		if (!IsReservedTypeID(First))
		{
			return First;
		}

		const uint32 Second = ((Hash / 65535u) % 65535u) + 1u;
		return static_cast<FCrowdyTypeID>(IsReservedTypeID(Second) ? 65535u : Second);
	}

	// Maps into 1..65535; CROWDY_INVALID_TYPE_ID is never produced.
	static FCrowdyTypeID GenerateFromStruct(const UScriptStruct* Struct)
	{
		check(Struct);
		return TypeIDFromHash(HashNameUtf8(Struct->GetPathName()));
	}

	// GenerateFromStruct for an event payload: never a reserved number.
	static FCrowdyTypeID GenerateEventFromStruct(const UScriptStruct* Struct)
	{
		check(Struct);
		return EventTypeIDFromHash(HashNameUtf8(Struct->GetPathName()));
	}

	static bool WouldCollide(FCrowdyTypeID ID, const UScriptStruct* Incoming, const TMap<FCrowdyTypeID, TObjectPtr<UScriptStruct>>& Existing)
	{
		const TObjectPtr<UScriptStruct>* Found = Existing.Find(ID);
		return Found && Found->Get() != Incoming;
	}

	// Renaming or moving a class changes its ID. Acceptable because spawn
	// events are transient; anything persisting spawn data must store the
	// class path instead. Blueprint class paths include the _C suffix.
	// CROWDY_INVALID_CLASS_ID is never produced.
	static FCrowdyClassID GenerateFromClass(const UClass* Class)
	{
		check(Class);
		const uint32 Hash = HashNameUtf8(Class->GetPathName());
		return Hash == CROWDY_INVALID_CLASS_ID ? 1u : Hash;
	}

	// Stable 64-bit hash of an arbitrary string, used for RPC FunctionIDs where the
	// whole signature is hashed so any drift produces a different ID.
	static int64 GenerateFromString(const FString& String)
	{
		const uint64 Hash = HashNameUtf8_64(String);
		return Hash == 0 ? 1 : static_cast<int64>(Hash);
	}
};
