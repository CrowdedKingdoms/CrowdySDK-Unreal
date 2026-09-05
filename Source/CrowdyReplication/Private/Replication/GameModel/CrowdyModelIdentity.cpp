#include "Replication/GameModel/CrowdyModelIdentity.h"

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/HelperFunctions.h"
#include "Hash/CityHash.h"
#include "Engine/World.h"

FString FCrowdyModelIdentity::NetIDToContainerKey(const FGuid& NetID)
{
	return NetID.ToString(EGuidFormats::Digits).ToLower();
}

FGuid FCrowdyModelIdentity::StableNetIDFromActorPath(const FString& ActorPathName)
{
	// The PIE prefix is stripped so PIE clients in one process, packaged clients, and an edit-time scan of
	// the same level all hash the identical path.
	const FString StablePath = UWorld::RemovePIEPrefix(ActorPathName);
	const uint64 PathHash = CityHash64(
		reinterpret_cast<const char*>(*StablePath), StablePath.Len() * sizeof(TCHAR));
	return UHelperFunctions::GetDeterministicID(static_cast<int64>(PathHash));
}

FGuid FCrowdyModelIdentity::NetIDFromBindingKey(const FString& Key)
{
	return UHelperFunctions::GetDeterministicID(FCrowdyTypeIDGenerator::GenerateFromString(Key));
}

bool FCrowdyModelIdentity::ContainerKeyToNetID(const FString& Key, FGuid& OutNetID)
{
	// EGuidFormats::Digits validates length==32 and every character is a hex digit before parsing
	// (case-insensitively), returning false otherwise -- exactly the lossless-inverse-with-clean-
	// rejection this needs, so no bespoke parser.
	return FGuid::ParseExact(Key, EGuidFormats::Digits, OutNetID);
}

bool FCrowdyModelIdentity::TryLocalUserIDForOwner(const UCrowdyGameSession* Session, const FGuid& OwnerID, int64& OutUserID)
{
	OutUserID = 0;
	if (!IsValid(Session) || OwnerID != Session->GetID())
	{
		return false;
	}

	OutUserID = Session->GetUserID();
	return true;
}
