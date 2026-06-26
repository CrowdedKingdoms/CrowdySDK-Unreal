#pragma once
#include "CoreMinimal.h"
#include "Hash/CityHash.h"
#include "Core/FCrowdyTypeID.h"

struct FCrowdyTypeIDGenerator
{
	static FCrowdyTypeID GenerateFromStruct(const UScriptStruct* Struct)
	{
		check(Struct);
		const FString Path = Struct->GetPathName();
		const uint32 Hash = FCrc::MemCrc32(*Path, Path.Len() * sizeof(TCHAR));
		return static_cast<FCrowdyTypeID>((Hash % 65535u) + 1u);
	}
	
	static bool WouldCollide(FCrowdyTypeID ID, const UScriptStruct* Incoming, const TMap<FCrowdyTypeID, TObjectPtr<UScriptStruct>>& Existing)
	{
		const TObjectPtr<UScriptStruct>* Found = Existing.Find(ID);
		return Found && Found->Get() != Incoming;
	}

	// Renaming or moving a class changes its ID. Acceptable because spawn
	// events are transient; anything persisting spawn data must store the
	// class path instead. Blueprint class paths include the _C suffix.
	static FCrowdyClassID GenerateFromClass(const UClass* Class)
	{
		check(Class);
		const FString Path = Class->GetPathName();
		const uint32 Hash = FCrc::MemCrc32(*Path, Path.Len() * sizeof(TCHAR));
		return Hash == CROWDY_INVALID_CLASS_ID ? 1u : Hash;
	}

	// Stable 64-bit hash of an arbitrary string, used for RPC FunctionIDs where the
	// whole signature is hashed so any drift produces a different ID. Hashes the
	// UTF-8 bytes (not raw TCHAR) so the value matches across platforms regardless
	// of wide-char width.
	static int64 GenerateFromString(const FString& String)
	{
		const FTCHARToUTF8 Utf8(*String);
		const uint64 Hash = CityHash64(reinterpret_cast<const char*>(Utf8.Get()), Utf8.Length());
		return Hash == 0 ? 1 : static_cast<int64>(Hash);
	}
};
