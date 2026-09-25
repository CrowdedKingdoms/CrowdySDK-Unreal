// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// One server operation's calls and issue-to-callback latency. The sdk and http percentiles are over requests handed
// off and not canceled; 0 when there were none.
struct FCrowdyGameModelOpStatsRow
{
	FString Operation;
	int32 Calls = 0;
	int32 Failures = 0;
	// Requests sent again after a busy refusal, and requests that succeeded after at least one of those.
	int32 Retries = 0;
	int32 RecoveredByRetry = 0;
	double P50Ms = 0.0;
	double P95Ms = 0.0;
	double MaxMs = 0.0;
	// Request created to handed to the engine HTTP module.
	double SdkP50Ms = 0.0;
	double SdkP95Ms = 0.0;
	// Handed off to completion delivered, including any wait inside the HTTP module.
	double HttpP50Ms = 0.0;
	double HttpP95Ms = 0.0;
	// Failures by reason (server code, "http <status>", "canceled", "transport", ...), most frequent first.
	TArray<TPair<FString, int32>> FailureReasons;
};

// Bytes and responses the HTTP transport has seen since the last reset, and the requests it holds open now and at
// most since then.
struct FCrowdyGameModelTransportTotals
{
	int64 RequestBytes = 0;
	int64 ResponseBytes = 0;
	int32 Responses = 0;
	int32 InFlight = 0;
	int32 PeakInFlight = 0;
};

// Plain diagnostics about how the Game Model uses the network; never read to make a decision.
struct CROWDYREPLICATION_API FCrowdyGameModelNetStats
{
	double SinceSeconds = 0.0;
	int32 PullRequests = 0;
	int32 PulledRows = 0;
	int32 PullApplies = 0;
	int32 RedundantPullApplies = 0;
	// Pulls whose result is applied, sent while another such pull of the same container was out; plain reads excluded.
	int32 OverlappingPulls = 0;
	// Pulls held behind one in flight, follow-ups sent, and differing pulled keys left alone as older than an invoke's write.
	int32 PullsHeldBehindInFlight = 0;
	int32 FollowUpPulls = 0;
	int32 StaleKeysSkipped = 0;
	int32 SelfInvokesMarked = 0;
	int32 SelfEchoesDropped = 0;
	// Hints that pulled within a window of a self-invoke, including one whose mark was given back.
	int32 HintsAfterSelfInvokeNotDropped = 0;
	// Self-invoke marks that expired unclaimed: echoes that never came or came late, or attempts still out past the window.
	int32 SelfEchoMarksExpired = 0;
	// Containers with a receiver sent to the pull path by the backstop, one per container per round; a round fires a
	// window after its first drop, so a later drop in the same round is pulled sooner than that.
	int32 SelfEchoBackstopPulls = 0;
	int32 ResolveStarts = 0;
	int32 ReResolves = 0;
	// Pages the bulk resolve's paged lists read; a keyed read is a GameModelContainers call too, but not one of these.
	int32 BulkResolveListPages = 0;
	// Invoke attempts that started, counted where the governor's ledger is written.
	int32 InvokesDispatched = 0;
	// Busy invokes sent again, invokes that succeeded after one, and busy refusals handed back or dropped at teardown.
	int32 InvokeBusyRetries = 0;
	int32 InvokeBusyRecovered = 0;
	int32 InvokeBusyGaveUp = 0;
	TMap<FString, int32> PullsByContainer;
	TMap<FString, int32> InvokeFaults;

	// Zeroes the counters and the two reported maps; the bookkeeping below is kept.
	void Reset(double NowSeconds);

	// The server names fault codes and container ids, so the maps keyed by them are bounded.
	static constexpr int32 MaxInvokeFaultKeys = 32;
	static constexpr int32 MaxPulledContainerKeys = 256;
	static constexpr int32 MaxFaultKeyPartChars = 48;
	static constexpr int32 MaxEverBound = 16384;
	static constexpr const TCHAR* OverflowKey = TEXT("other");

	// Adds one to Key, or to OverflowKey once Counts already holds MaxKeys distinct keys.
	static void CountBounded(TMap<FString, int32>& Counts, const FString& Key, int32 MaxKeys);

	// "<code or none> blame=<word> retryable=<0|1> <thrown|in-band>", code and blame clipped to MaxFaultKeyPartChars.
	static FString MakeInvokeFaultKey(const FString& FaultCode, const FString& BlameWord, bool bRetryable, bool bTransportOk);

	// "CODE xN, CODE xM" in the given order; empty for no reasons.
	static FString FormatFailureReasons(const TArray<TPair<FString, int32>>& Reasons);

	// Nearest-rank percentile, Fraction in [0,1]; 0 for no samples.
	static double Percentile(TArray<double> SamplesMs, double Fraction);

	TMap<FString, int32> InFlightPulls;
	TMap<FString, double> LastSelfInvokeSeconds;
	// Adds NetID to EverBound, or sets bEverBoundFull once it holds MaxEverBound entities.
	void RecordBound(const FGuid& NetID);

	// Entities bound in this world, up to MaxEverBound; survives Reset.
	TSet<FGuid> EverBound;
	bool bEverBoundFull = false;
	// Invoke attempt times, oldest first, pruned to the allowance window.
	TArray<double> RecentInvokeSeconds;
};
