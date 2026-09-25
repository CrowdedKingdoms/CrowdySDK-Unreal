// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyGameModelNetStats.h"

void FCrowdyGameModelNetStats::Reset(double NowSeconds)
{
	SinceSeconds = NowSeconds;
	PullRequests = 0;
	PulledRows = 0;
	PullApplies = 0;
	RedundantPullApplies = 0;
	OverlappingPulls = 0;
	PullsHeldBehindInFlight = 0;
	FollowUpPulls = 0;
	StaleKeysSkipped = 0;
	SelfInvokesMarked = 0;
	SelfEchoesDropped = 0;
	HintsAfterSelfInvokeNotDropped = 0;
	SelfEchoMarksExpired = 0;
	SelfEchoBackstopPulls = 0;
	ResolveStarts = 0;
	ReResolves = 0;
	BulkResolveListPages = 0;
	InvokesDispatched = 0;
	InvokeBusyRetries = 0;
	InvokeBusyRecovered = 0;
	InvokeBusyGaveUp = 0;
	PullsByContainer.Reset();
	InvokeFaults.Reset();
}

void FCrowdyGameModelNetStats::CountBounded(TMap<FString, int32>& Counts, const FString& Key, int32 MaxKeys)
{
	if (int32* Existing = Counts.Find(Key))
	{
		++*Existing;
		return;
	}
	if (Counts.Num() >= MaxKeys)
	{
		++Counts.FindOrAdd(FString(OverflowKey));
		return;
	}
	Counts.Add(Key, 1);
}

void FCrowdyGameModelNetStats::RecordBound(const FGuid& NetID)
{
	if (EverBound.Num() >= MaxEverBound && !EverBound.Contains(NetID))
	{
		bEverBoundFull = true;
		return;
	}
	EverBound.Add(NetID);
}

FString FCrowdyGameModelNetStats::MakeInvokeFaultKey(const FString& FaultCode, const FString& BlameWord, bool bRetryable,
	bool bTransportOk)
{
	const FString Code = FaultCode.IsEmpty() ? FString(TEXT("none")) : FaultCode.Left(MaxFaultKeyPartChars);
	return FString::Printf(TEXT("%s blame=%s retryable=%d %s"), *Code, *BlameWord.Left(MaxFaultKeyPartChars),
		bRetryable ? 1 : 0, bTransportOk ? TEXT("in-band") : TEXT("thrown"));
}

FString FCrowdyGameModelNetStats::FormatFailureReasons(const TArray<TPair<FString, int32>>& Reasons)
{
	TArray<FString> Parts;
	Parts.Reserve(Reasons.Num());
	for (const TPair<FString, int32>& Reason : Reasons)
	{
		Parts.Add(FString::Printf(TEXT("%s x%d"), *Reason.Key, Reason.Value));
	}
	return FString::Join(Parts, TEXT(", "));
}

double FCrowdyGameModelNetStats::Percentile(TArray<double> SamplesMs, double Fraction)
{
	if (SamplesMs.IsEmpty())
	{
		return 0.0;
	}
	SamplesMs.Sort();
	const int32 Rank = FMath::CeilToInt32(FMath::Clamp(Fraction, 0.0, 1.0) * SamplesMs.Num());
	return SamplesMs[FMath::Clamp(Rank, 1, SamplesMs.Num()) - 1];
}
