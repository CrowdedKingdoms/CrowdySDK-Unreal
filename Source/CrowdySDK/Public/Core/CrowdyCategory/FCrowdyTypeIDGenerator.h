#pragma once
#include "CoreMinimal.h"
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
};
