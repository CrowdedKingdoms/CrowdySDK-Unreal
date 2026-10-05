#pragma once

#include "CoreMinimal.h"
#include "Subsystem/CrowdyTeams.h"

/** What the Crowdy Server Object component and the Server Object Link share. */
namespace CrowdyServerObjectShared
{
	inline const TCHAR* const WaitingForSignIn = TEXT("Waiting for the player to sign in");
	inline const TCHAR* const WaitingForTeams = TEXT("Waiting for the player's teams");

	/** Fills the teams cache; its change event carries the answer. */
	inline void RequestMyTeams(UCrowdyTeams* Teams)
	{
		if (Teams)
		{
			Teams->GetMyTeams(FOnMyTeamsSuccess(), FOnTeamError());
		}
	}

	/** Team Id when set, otherwise the player's only team in Memberships, null while unknown; empty with OutError saying why. */
	inline FString ResolvePlayersTeam(const TArray<FCrowdyTeamMembership>* Known, int64 TeamId, FString& OutError)
	{
		if (TeamId != 0)
		{
			return LexToString(TeamId);
		}
		if (!Known)
		{
			OutError = WaitingForTeams;
			return FString();
		}
		const TArray<FCrowdyTeamMembership>& Memberships = *Known;
		if (Memberships.IsEmpty())
		{
			OutError = TEXT("This player is in no Crowdy Team");
			return FString();
		}
		if (Memberships.Num() > 1)
		{
			OutError = FString::Printf(TEXT("This player is in %d teams; set Team Id"), Memberships.Num());
			return FString();
		}
		return LexToString(Memberships[0].Team.TeamId);
	}

	/** ResolvePlayersTeam from the teams cache. */
	inline FString ResolvePlayersTeam(const UCrowdyTeams* Teams, int64 TeamId, FString& OutError)
	{
		const bool bKnown = Teams && Teams->HasCachedTeams();
		const TArray<FCrowdyTeamMembership> Cached = bKnown ? Teams->GetCachedMyTeams() : TArray<FCrowdyTeamMembership>();
		return ResolvePlayersTeam(bKnown ? &Cached : nullptr, TeamId, OutError);
	}
}
