#include "Replication/GameModel/CrowdyModelIdentityProvider.h"

#include "CrowdyGameModelLog.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Subsystem/CrowdyGameSession.h"

FCrowdyModelIdentityProvider::FCrowdyModelIdentityProvider(UWorld* InWorld)
{
	if (InWorld)
	{
		EntitySubsystem = InWorld->GetSubsystem<UCrowdyEntitySubsystem>();
		ModelSubsystem = InWorld->GetSubsystem<UCrowdyGameModelSubsystem>();
		if (UGameInstance* GameInstance = InWorld->GetGameInstance())
		{
			GameSession = GameInstance->GetSubsystem<UCrowdyGameSession>();
		}
	}
	LogConstruction();
}

FCrowdyModelIdentityProvider::FCrowdyModelIdentityProvider(UCrowdyEntitySubsystem* InEntitySubsystem, UCrowdyGameSession* InGameSession)
	: EntitySubsystem(InEntitySubsystem)
	, GameSession(InGameSession)
{
	LogConstruction();
}

void FCrowdyModelIdentityProvider::LogConstruction() const
{
	UE_CLOG(CrowdyGameModelTrace::GameModel(), LogCrowdyGameModel, Log,
		TEXT("[CrowdyModelIdentity] Provider constructed (entity subsystem %s, game session %s)."),
		EntitySubsystem ? TEXT("resolved") : TEXT("null"),
		GameSession ? TEXT("resolved") : TEXT("null"));
}

bool FCrowdyModelIdentityProvider::GetNetIDForParticipant(const UObject* Participant, FGuid& OutNetID) const
{
	if (!EntitySubsystem || !Participant)
	{
		return false;
	}

	const FGuid Found = EntitySubsystem->FindEntityID(Participant);
	if (!Found.IsValid())
	{
		return false;
	}

	OutNetID = Found;
	return true;
}

bool FCrowdyModelIdentityProvider::IsLocallyOwned(const FGuid& NetID) const
{
	return EntitySubsystem && EntitySubsystem->IsLocallyOwned(NetID);
}

bool FCrowdyModelIdentityProvider::TryGetLocalUserId(int64& OutUserId) const
{
	OutUserId = 0;
	if (!GameSession)
	{
		return false;
	}

	const int64 UserId = GameSession->GetUserID();
	if (UserId <= 0)
	{
		return false;
	}

	OutUserId = UserId;
	return true;
}

bool FCrowdyModelIdentityProvider::TryGetCachedOwnerUserId(const FGuid& NetID, int64& Out) const
{
	// Delegates to the container subsystem's NetID -> ownerUserId cache, which is filled from the ensure/read result
	// that bound each entity's container (so it answers for a NetID once that entity has resolved locally).
	if (ModelSubsystem)
	{
		return ModelSubsystem->TryGetCachedOwnerUserId(NetID, Out);
	}
	Out = 0;
	return false;
}

FGuid FCrowdyModelIdentityProvider::GetHostID() const
{
	return EntitySubsystem ? EntitySubsystem->GetHostID() : FGuid();
}
