#include "Replication/Components/CrowdyEntityIdentity.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Utils/HelperFunctions.h"
#include "Hash/CityHash.h"
#include "Engine/World.h"

FGuid FCrowdyEntityIdentity::StableNetIDFromActorPath(const FString& ActorPathName)
{
	// The PIE prefix is stripped so PIE clients in one process, packaged clients, and an edit-time scan of
	// the same level all hash the identical path.
	const FString StablePath = UWorld::RemovePIEPrefix(ActorPathName);
	const uint64 PathHash = CityHash64(
		reinterpret_cast<const char*>(*StablePath), StablePath.Len() * sizeof(TCHAR));
	return UHelperFunctions::GetDeterministicID(static_cast<int64>(PathHash));
}

FGuid FCrowdyEntityIdentity::NetIDFromBindingKey(const FString& Key)
{
	return UHelperFunctions::GetDeterministicID(FCrowdyTypeIDGenerator::GenerateFromString(Key));
}
