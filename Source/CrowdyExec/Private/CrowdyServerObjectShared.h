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

	/** The chunk holding Location on the ground, as actor updates place it: Unreal's X and Y each divided by ChunkSize, rounded down. False for none. */
	inline bool GroundChunkAt(const FVector& Location, double ChunkSize, int64& OutX, int64& OutY)
	{
		if (ChunkSize <= 0.0)
		{
			return false;
		}
		const double X = FMath::FloorToDouble(Location.X / ChunkSize);
		const double Y = FMath::FloorToDouble(Location.Y / ChunkSize);
		constexpr double Limit = 1.0e15;
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || FMath::Abs(X) > Limit || FMath::Abs(Y) > Limit)
		{
			return false;
		}
		OutX = static_cast<int64>(X);
		OutY = static_cast<int64>(Y);
		return true;
	}

	/** ResolvePlayersTeam from the teams cache. */
	inline FString ResolvePlayersTeam(const UCrowdyTeams* Teams, int64 TeamId, FString& OutError)
	{
		const bool bKnown = Teams && Teams->HasCachedTeams();
		const TArray<FCrowdyTeamMembership> Cached = bKnown ? Teams->GetCachedMyTeams() : TArray<FCrowdyTeamMembership>();
		return ResolvePlayersTeam(bKnown ? &Cached : nullptr, TeamId, OutError);
	}
}
