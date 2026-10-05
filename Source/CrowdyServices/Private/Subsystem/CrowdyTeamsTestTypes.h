#pragma once

#include "CoreMinimal.h"
#include "Queries/Data/Teams/Types/FCrowdyTeam.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamError.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMember.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamMembership.h"
#include "UObject/Object.h"
#include "CrowdyTeamsTestTypes.generated.h"

/** Records what the teams subsystem hands the delegates bound to it, as a Blueprint graph would receive it. */
UCLASS(Transient)
class UCrowdyTeamsTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<TArray<FCrowdyTeamMembership>> CacheEvents;
	TArray<TArray<FCrowdyTeamMembership>> MyTeamsAnswers;
	TArray<FString> Errors;
	int32 Successes = 0;

	UFUNCTION()
	void HandleCacheChanged(const TArray<FCrowdyTeamMembership>& Memberships) { CacheEvents.Add(Memberships); }

	UFUNCTION()
	void HandleMyTeams(const TArray<FCrowdyTeamMembership>& Memberships) { MyTeamsAnswers.Add(Memberships); }

	UFUNCTION()
	void HandleTeam(FCrowdyTeam Team) { ++Successes; }

	UFUNCTION()
	void HandleMember(FCrowdyTeamMember Member) { ++Successes; }

	UFUNCTION()
	void HandleDone() { ++Successes; }

	UFUNCTION()
	void HandleError(FCrowdyTeamError Error, FString Message) { Errors.Add(Message); }
};
