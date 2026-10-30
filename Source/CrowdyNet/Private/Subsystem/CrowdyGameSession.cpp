// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystem/CrowdyGameSession.h"
#include "CrowdyNetLog.h"
#include "Async/TaskGraphInterfaces.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

void UCrowdyGameSession::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_CLOG(CrowdyNetTrace::Net(), LogCrowdyNet, Log, TEXT("Game Session Initialized."))
}

void UCrowdyGameSession::Deinitialize()
{
	Super::Deinitialize();
}

FInt64Vector UCrowdyGameSession::GetLocalPlayerChunk(const UWorld* World) const
{
	FInt64Vector Chunk = GameSessionInfo.CurrentPlayerChunkCoordinates;
	TryGetLocalPlayerChunk(World, Chunk);
	return Chunk;
}

bool UCrowdyGameSession::TryGetLocalPlayerChunk(const UWorld* World, FInt64Vector& OutChunk) const
{
	const APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	const APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
	const double ChunkSize = GameSessionInfo.ChunkSize;
	if (!Pawn || ChunkSize <= 0.0)
	{
		return false;
	}

	const FVector Location = Pawn->GetActorLocation();
	OutChunk = FInt64Vector(
		static_cast<int64>(FMath::FloorToDouble(Location.X / ChunkSize)),
		static_cast<int64>(FMath::FloorToDouble(Location.Y / ChunkSize)),
		static_cast<int64>(FMath::FloorToDouble(Location.Z / ChunkSize)));
	return true;
}

void UCrowdyGameSession::SetAppPaused(const bool bPaused, const FString& Reason)
{
	if (bAppPaused == bPaused && AppPausedReason.Equals(Reason, ESearchCase::CaseSensitive))
	{
		return;
	}
	bAppPaused = bPaused;
	AppPausedReason = Reason;
	OnAppPausedChanged.Broadcast(bPaused, Reason);
}

void UCrowdyGameSession::SetAccessEnded(const FString& Reason, const FString& BlockedUntil)
{
	// A timed block's end can arrive after the connection already closed with none, and is worth one more announcement.
	const bool bLateBlockEnd = bAccessEnded && AccessBlockedUntil.IsEmpty() && !BlockedUntil.IsEmpty();
	if (bAccessEnded && !bLateBlockEnd)
	{
		return;
	}
	bAccessEnded = true;
	AccessEndedReason = Reason;
	AccessBlockedUntil = BlockedUntil;
	OnAccessEnded.Broadcast(Reason, BlockedUntil);
}

void UCrowdyGameSession::ClearAccessEnded()
{
	bAccessEnded = false;
	AccessEndedReason.Empty();
	AccessBlockedUntil.Empty();
}
