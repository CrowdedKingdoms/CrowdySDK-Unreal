// Fill out your copyright notice in the Description page of Project Settings.


#include "Subsystem/CrowdyGameSession.h"
#include "CrowdyNetLog.h"
#include "Async/TaskGraphInterfaces.h"

void UCrowdyGameSession::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_CLOG(CrowdyNetTrace::Net(), LogCrowdyNet, Log, TEXT("Game Session Initialized."))
}

void UCrowdyGameSession::Deinitialize()
{
	Super::Deinitialize();
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
