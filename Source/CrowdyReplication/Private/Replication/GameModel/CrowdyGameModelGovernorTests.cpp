#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"

// The per-player invoke budget governor: how much of the sliding window is spent, how far a coalesce window
// may stretch to buy allowance back, and which refusals are safe to repeat. Every function here is pure and
// static (or drives the ledger through a test-only seam), so the governor's own arithmetic is provable with no
// world, no server and no timer.
//
// Two things here are NOT provable headless. TryScheduleBudgetRetry actually arming and firing a retry end to end
// needs a world and a timer manager, so only its decision inputs are covered here; that a real refusal decodes into
// those inputs is proved separately against the bridge in CrowdySDK.CrowdyCpp.InvokeRateLimitRefusal, off a canned
// response. And the point where a Game API call is COUNTED sits where the client is resolved, which needs a game
// instance hosting a real client: headlessly every call fails before it gets there, so nothing is ever counted. Both
// need a live PIE/network gate; this suite covers the decision functions they are built from, and the ledger they
// write into.
namespace
{
	constexpr EAutomationTestFlags CrowdyGovernorTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Shaped like a refusal really arrives: THROWN, so bTransportOk is false and there is no gameModelInvoke object.
	// The attribution is the only thing separating this from a dropped connection, which is exactly what the gate
	// under test has to rely on.
	FCrowdyInvokeResult MakeRefusal(ECrowdyPlayerFaultBlame Blame, const FString& FaultCode)
	{
		FCrowdyInvokeResult Result;
		Result.bTransportOk = false;
		Result.bSuccess = false;
		Result.Blame = Blame;
		Result.FaultCode = FaultCode;
		return Result;
	}
}

// StretchCoalesceWindowSeconds only stretches a window when there is pressure to relieve: below half the
// allowance spent, an effect's authored window is returned byte-for-byte unchanged. Mutating away the
// ReliefPoint gate (or the min() against MaxCoalesceWindowSeconds) turns the first two cases here red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCoalesceWindowUnstretchedBelowReliefTest,
	"CrowdySDK.GameModel.CoalesceWindowUnstretchedBelowRelief", CrowdyGovernorTestFlags)
bool FCrowdyGameModelCoalesceWindowUnstretchedBelowReliefTest::RunTest(const FString& Parameters)
{
	// Exactly at half the allowance (the relief point itself) is still "not under pressure": the window is
	// returned unchanged.
	TestEqual(TEXT("half the allowance spent leaves the authored window unchanged"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(0.5f, 60, 120), 0.5f);

	// No traffic recorded at all is the same answer for a different reason (RecentInvokes <= 0 short-circuits).
	TestEqual(TEXT("no recorded traffic leaves the authored window unchanged"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(0.5f, 0, 120), 0.5f);

	// A caller with no meaningful limit (Limit <= 0) cannot be under pressure either, however much traffic is
	// reported against it.
	TestEqual(TEXT("a non-positive limit also leaves the window unchanged, whatever the traffic"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(0.5f, 1000, 0), 0.5f);

	// An authored window at or past the absolute ceiling is bounded down to it even with zero pressure: the
	// ceiling is unconditional, not just the top of the stretch curve.
	TestEqual(TEXT("an authored window past the ceiling is capped even with no pressure"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(5.0f, 0, 120), 2.0f);

	// A window of zero or negative seconds means "do not merge at all", not "merge instantly".
	TestEqual(TEXT("a zero authored window stays zero"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(0.0f, 90, 120), 0.0f);
	TestEqual(TEXT("a negative authored window stays zero"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(-1.0f, 90, 120), 0.0f);

	return true;
}

// Past the relief point the window grows linearly towards MaxCoalesceStretch, bounded above by
// MaxCoalesceWindowSeconds and never below the effect's own authored window. Mutating the Stretch formula's
// slope, the ReliefPoint constant, or either Clamp bound turns one of these three exact values red without
// touching any other.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCoalesceWindowStretchesUnderPressureTest,
	"CrowdySDK.GameModel.CoalesceWindowStretchesUnderPressure", CrowdyGovernorTestFlags)
bool FCrowdyGameModelCoalesceWindowStretchesUnderPressureTest::RunTest(const FString& Parameters)
{
	// 75% of a 120-call/10s allowance spent (halfway between the relief point and full): the stretch factor is
	// 4.5x, so a 0.25s authored window becomes 1.125s exactly.
	TestEqual(TEXT("three-quarters pressure stretches the window by the expected exact factor"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(0.25f, 90, 120), 1.125f);

	// A fully spent allowance stretches an effect's authored window as far as the curve allows, which is
	// immediately clamped down to the absolute ceiling (0.5s * 8x = 4.0s, capped to 2.0s).
	TestEqual(TEXT("full pressure clamps the stretched window to the absolute ceiling"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(0.5f, 120, 120), 2.0f);

	// The stretch curve only ever grows the window: at full pressure it must still never fall below the
	// authored value it started from, here proven with an authored window small enough that 8x of it is still
	// under the ceiling (0.1s * 8x = 0.8s).
	TestEqual(TEXT("full pressure stretches a small authored window, never shrinks it"),
		UCrowdyGameModelSubsystem::StretchCoalesceWindowSeconds(0.1f, 120, 120), 0.8f);

	return true;
}

// The governor's view of "how much of the allowance is spent right now": a sliding InvokeBudgetWindowSeconds
// window counted from the real clock, fed by whatever RecordInvokeAttempt has written. Mutating the ledger scan
// in GetRecentInvokeCount to use <= instead of < (or reading the wrong window constant) turns this boundary
// case red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelRecentInvokeWindowTest,
	"CrowdySDK.GameModel.RecentInvokeWindow", CrowdyGovernorTestFlags)
bool FCrowdyGameModelRecentInvokeWindowTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	TestEqual(TEXT("a fresh ledger counts nothing"), Model->GetRecentInvokeCount(), 0);

	// FApp::GetCurrentTime() only advances when something ticks the engine loop, which nothing here does, so it
	// reads as one stable value for the whole of this synchronous test: every attempt below is written relative
	// to that one anchor, oldest first (matching how the real ledger only ever grows forward in time).
	const double RealNow = FApp::GetCurrentTime();
	const double Window = static_cast<double>(UCrowdyGameModelSubsystem::InvokeBudgetWindowSeconds);

	Model->RecordInvokeAttemptForTest(RealNow - Window - 0.1); // just outside the window
	Model->RecordInvokeAttemptForTest(RealNow - Window + 0.1); // just inside the window
	Model->RecordInvokeAttemptForTest(RealNow);                // right now

	TestEqual(TEXT("only the attempts still inside the sliding window are counted"),
		Model->GetRecentInvokeCount(), 2);

	return true;
}

// ResolveBudgetRetryDelaySeconds: a server-suggested delay is clamped into [Min, Max] and always wins; with no
// suggestion, a doubling local backoff runs, itself clamped so a caller with a large attempt index can never
// wait indefinitely. Mutating either clamp, the doubling base, or the "suggestion wins" precedence turns one of
// these red without the others moving.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBudgetRetryDelayTest,
	"CrowdySDK.GameModel.BudgetRetryDelay", CrowdyGovernorTestFlags)
bool FCrowdyGameModelBudgetRetryDelayTest::RunTest(const FString& Parameters)
{
	const TOptional<int64> NoSuggestion;

	// No suggestion: a doubling backoff from the floor, clamped at the ceiling once doubling would exceed it.
	TestEqual(TEXT("attempt 0 backs off at the floor"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(0, NoSuggestion), 1.0f);
	TestEqual(TEXT("attempt 1 doubles"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(1, NoSuggestion), 2.0f);
	TestEqual(TEXT("attempt 2 doubles again"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(2, NoSuggestion), 4.0f);
	TestEqual(TEXT("attempt 3 doubles a third time"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(3, NoSuggestion), 8.0f);
	TestEqual(TEXT("attempt 4 would double past the ceiling, so it is clamped to it"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(4, NoSuggestion), 11.0f);
	TestEqual(TEXT("a far larger attempt index never grows past the ceiling either"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(100, NoSuggestion), 11.0f);

	// The ceiling is not a free parameter. retryAfterMs is what REMAINS of the budget window, so a refusal arriving at
	// the very start of one names nearly the whole window; a ceiling below that would clamp the wait short and send
	// the retry into a window still shut, spending one of only two retries on a call that cannot succeed.
	//
	// This assertion states the RELATIONSHIP, which the numeric cases below cannot: they are absolute values, so
	// lowering the ceiling moves them too and they would all have to be re-fitted around whatever the new number is.
	// This one refuses the new number itself, and says why.
	TestTrue(TEXT("the retry ceiling can express a wait as long as the whole budget window"),
		UCrowdyGameModelSubsystem::MaxBudgetRetryDelaySeconds
			>= static_cast<float>(UCrowdyGameModelSubsystem::InvokeBudgetWindowSeconds));

	// A server suggestion wins over the local backoff regardless of attempt index, but is clamped into the same
	// bounds so a hostile or mis-set suggestion cannot park a caller far outside them.
	TestEqual(TEXT("a suggestion within bounds is used as-is"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(3, TOptional<int64>(3000)), 3.0f);
	TestEqual(TEXT("a suggestion below the floor is clamped up to it"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(0, TOptional<int64>(500)), 1.0f);
	TestEqual(TEXT("a suggestion above the ceiling is clamped down to it"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(0, TOptional<int64>(20000)), 11.0f);

	// The case the ceiling was raised for: a refusal built at the very start of a 10s window names most of it, and
	// that wait must survive intact rather than being clamped into a retry the window is still shut for.
	TestEqual(TEXT("a wait spanning almost the whole budget window is honoured, not clamped"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(0, TOptional<int64>(9800)), 9.8f);

	// Zero versus unset, which is the distinction the optional exists for and the one a plain int would erase. Both
	// happen to wait the floor here, so the assertion that carries the meaning is the PAIRING: a set zero must take
	// the suggestion branch, and it proves it by ignoring the attempt index that the fallback would have honoured.
	TestEqual(TEXT("a suggestion of zero is honoured as a suggestion, clamped up to the floor"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(0, TOptional<int64>(0)), 1.0f);
	TestEqual(TEXT("and it stays the floor at an attempt index the local backoff would have doubled to the ceiling"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(3, TOptional<int64>(0)), 1.0f);
	TestEqual(TEXT("while UNSET at that same index falls through to the doubling backoff"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(3, NoSuggestion), 8.0f);

	// A duration cannot run backwards, so a negative suggestion is nothing the server can have meant and reads as
	// unset. Letting it through the clamp would return the floor and silently discard the local backoff instead.
	TestEqual(TEXT("a negative suggestion falls back to the local backoff rather than clamping to the floor"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(3, TOptional<int64>(-5000)), 8.0f);

	// A hostile value near the int64 limit must clamp to the ceiling like any other overlong wait. Converting it
	// through float before the clamp, or narrowing it to int32 on the way in, is how a huge number becomes a small
	// or negative one and parks (or fails to park) the caller for the wrong duration.
	TestEqual(TEXT("an absurd suggestion is clamped to the ceiling, not wrapped"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(0, TOptional<int64>(TNumericLimits<int64>::Max())), 11.0f);
	TestEqual(TEXT("and one just past the ceiling in milliseconds clamps there too"),
		UCrowdyGameModelSubsystem::ResolveBudgetRetryDelaySeconds(0, TOptional<int64>(11001)), 11.0f);

	return true;
}

// IsBudgetRefusal is the entire safety argument for ever retrying a refused invoke: only a result BOTH blamed
// on Budget AND carrying the server's own RATE_LIMITED fault code is safe to repeat, because that is the one
// refusal the server decides before the function runs. Every other attribution - Author (the identical call
// would fail identically), Unknown (nothing was reported, so nothing licenses a retry), and Platform (a
// different, unretried-here class of fault) - must read false even when paired with the same fault code, and a
// successful result must never read as a refusal at all. Mutating the Blame check to admit any blame, or
// dropping the FaultCode check, turns one of these cases red without the others moving.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBudgetRefusalBlameTest,
	"CrowdySDK.GameModel.BudgetRefusalBlame", CrowdyGovernorTestFlags)
bool FCrowdyGameModelBudgetRefusalBlameTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Budget + RATE_LIMITED is the one retryable refusal"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(MakeRefusal(ECrowdyPlayerFaultBlame::Budget, TEXT("RATE_LIMITED"))));

	TestTrue(TEXT("the fault code match is case-insensitive"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(MakeRefusal(ECrowdyPlayerFaultBlame::Budget, TEXT("rate_limited"))));

	TestFalse(TEXT("Budget blame with any other fault code is not retried"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(MakeRefusal(ECrowdyPlayerFaultBlame::Budget, TEXT("SOME_OTHER_CODE"))));

	TestFalse(TEXT("Blame Author must not be retried, even carrying the rate-limit code"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(MakeRefusal(ECrowdyPlayerFaultBlame::Author, TEXT("RATE_LIMITED"))));

	TestFalse(TEXT("Blame Unknown must not be retried, even carrying the rate-limit code"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(MakeRefusal(ECrowdyPlayerFaultBlame::Unknown, TEXT("RATE_LIMITED"))));

	TestFalse(TEXT("Blame Platform is a different, unretried-here class of fault"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(MakeRefusal(ECrowdyPlayerFaultBlame::Platform, TEXT("RATE_LIMITED"))));

	FCrowdyInvokeResult Success = MakeRefusal(ECrowdyPlayerFaultBlame::Budget, TEXT("RATE_LIMITED"));
	Success.bSuccess = true;
	TestFalse(TEXT("a successful result is never a refusal, whatever blame/code it happens to carry"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(Success));

	return true;
}

// What separates the one retryable refusal from a network failure, now that both arrive with bTransportOk false.
//
// This gate used to require a clean transport, on the reasoning that a failed one means the client never heard the
// server and the call may have committed unseen. The reasoning is sound and the premise was wrong: a rate-limit
// refusal is THROWN, so it never has a clean transport, and requiring one did not narrow the gate but closed it. The
// ATTRIBUTION carries that argument instead, and carries it better - Blame and FaultCode are written only from a
// GraphQL error the server authored, so a dropped connection or a timeout leaves both empty and cannot be mistaken
// for a refusal, while a clean transport only ever proved that some result object came back.
//
// Restoring the bTransportOk guard turns the first case here red on its own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelBudgetRefusalIsAttributedTest,
	"CrowdySDK.GameModel.BudgetRefusalIsAttributed", CrowdyGovernorTestFlags)
bool FCrowdyGameModelBudgetRefusalIsAttributedTest::RunTest(const FString& Parameters)
{
	// The real shape: thrown, so no clean transport, and retried on the strength of what the server said.
	FCrowdyInvokeResult Thrown = MakeRefusal(ECrowdyPlayerFaultBlame::Budget, TEXT("RATE_LIMITED"));
	TestFalse(TEXT("a thrown refusal has no clean transport"), Thrown.bTransportOk);
	TestTrue(TEXT("and is still the one retryable refusal, because the server attributed it"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(Thrown));

	// A dropped connection or a timeout: the same failed transport, no attribution, never retried. This is the case
	// the old transport guard existed to catch, and it is caught here by the absence of the server's own words.
	FCrowdyInvokeResult NetworkFailure;
	NetworkFailure.bTransportOk = false;
	NetworkFailure.bSuccess = false;
	NetworkFailure.ErrorMessage = TEXT("connection reset");
	TestFalse(TEXT("an unattributed transport failure is not a refusal"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(NetworkFailure));

	// Half an attribution is not one. Either field alone leaves a failure that could be something else entirely, so
	// neither on its own may open the gate.
	FCrowdyInvokeResult CodeOnly;
	CodeOnly.bTransportOk = false;
	CodeOnly.FaultCode = TEXT("RATE_LIMITED");
	TestFalse(TEXT("a fault code with no blame is not a refusal"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(CodeOnly));

	FCrowdyInvokeResult BlameOnly;
	BlameOnly.bTransportOk = false;
	BlameOnly.Blame = ECrowdyPlayerFaultBlame::Budget;
	TestFalse(TEXT("a budget blame with no fault code is not a refusal"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(BlameOnly));

	// bRetryable is still not consulted: a failure claiming to be retryable without the budget attribution stays
	// outside the gate, so the server's generous default can never widen it.
	FCrowdyInvokeResult ClaimsRetryable;
	ClaimsRetryable.bTransportOk = false;
	ClaimsRetryable.bRetryable = true;
	ClaimsRetryable.Blame = ECrowdyPlayerFaultBlame::Platform;
	ClaimsRetryable.FaultCode = TEXT("PLATFORM_BUSY");
	TestFalse(TEXT("a retryable platform fault is still not a budget refusal"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(ClaimsRetryable));

	// The same attribution arriving IN BAND is refused, and this is the case that stops "not a clean transport" from
	// being read as "the transport does not matter". The server never reports a rate limit in band, so a result
	// carrying one alongside a live gameModelInvoke object contradicts the contract - and it is the shape where the
	// function actually RAN, so repeating it is the one way this retry could apply a write twice. Widening the gate
	// to ignore the transport instead of inverting it turns exactly this case red.
	FCrowdyInvokeResult InBand = MakeRefusal(ECrowdyPlayerFaultBlame::Budget, TEXT("RATE_LIMITED"));
	InBand.bTransportOk = true;
	TestFalse(TEXT("a rate-limit attribution returned in band is not retried, because the function ran"),
		UCrowdyGameModelSubsystem::IsBudgetRefusal(InBand));

	return true;
}

// The guard that stops a retry landing a dead life's writes on a live one.
//
// A container id is a deterministic function of the NetID, so an entity that unbinds and binds again resolves to the
// SAME row while being a different life. The coalescer already refuses to flush a merged window across that boundary,
// but a retry re-sends an ALREADY-FLUSHED request seconds later, long past the window's own 2s ceiling, so without
// its own check it walks straight around the one that exists. The retried payload can be a whole window's summed
// magnitude, which is why this matters more on the retry path than anywhere else.
//
// Deleting the epoch comparison in IsInvokeBindingStillValid turns the rebind case red; deleting the null check turns
// the unbind case red; making it return false unconditionally turns the two "still valid" cases red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelRetryBindingGuardTest,
	"CrowdySDK.GameModel.RetryBindingGuard", CrowdyGovernorTestFlags)
bool FCrowdyGameModelRetryBindingGuardTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	const FGuid NetID(1, 2, 3, 4);
	Model->BindEntityContainerForTest(NetID, TEXT("c-deterministic"));
	const uint32 Snapshot = Model->GetBindEpochForTest(NetID);
	TestTrue(TEXT("a binding is stamped with an epoch when it is made"), Snapshot != 0);

	TestTrue(TEXT("an untouched binding is still the one the invoke was dispatched under"),
		Model->IsInvokeBindingStillValidForTest(NetID, Snapshot));

	// The entity went away and never came back. There is no target left at all, so the retry has nothing to send to.
	Model->UnbindEntityContainerForTest(NetID);
	TestFalse(TEXT("an unbound entity fails the guard"),
		Model->IsInvokeBindingStillValidForTest(NetID, Snapshot));

	// The case the guard exists for, and the one a null check alone would miss: the entity respawned and bound to the
	// very same container row. Everything the request pinned still resolves, so nothing about the id can tell the two
	// lives apart. Only the epoch can.
	Model->BindEntityContainerForTest(NetID, TEXT("c-deterministic"));
	const uint32 Rebound = Model->GetBindEpochForTest(NetID);
	TestTrue(TEXT("a rebind to the same container is stamped with a NEW epoch"), Rebound != Snapshot);
	TestFalse(TEXT("a retry from the previous life fails the guard even though the container id still resolves"),
		Model->IsInvokeBindingStillValidForTest(NetID, Snapshot));
	TestTrue(TEXT("while an invoke dispatched under the new binding passes"),
		Model->IsInvokeBindingStillValidForTest(NetID, Rebound));

	// A container-addressed invoke names its target directly, so there is no binding that could move under it and the
	// guard must never block one. Gating on the guard without the unbound short-circuit turns this red.
	TestTrue(TEXT("an unbound (container-addressed) invoke is never blocked by the guard"),
		Model->IsUnboundInvokeBindingStillValidForTest());

	return true;
}

// The sliding-window ledger is a fixed-capacity ring, so recording a call is constant time however long the
// session has been running. What must survive that: entries still inside the window are counted, entries that
// aged out are not, and past the capacity the OLDEST entries are the ones dropped. Mutating the head advance in
// RecordInvokeAttempt (dropping the newest instead of the oldest, or failing to advance at all) turns the
// wrap-around case red; mutating the expiry walk turns the first case red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelInvokeLedgerRingTest,
	"CrowdySDK.GameModel.InvokeLedgerRing", CrowdyGovernorTestFlags)
bool FCrowdyGameModelInvokeLedgerRingTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}

	// FApp::GetCurrentTime() does not advance inside this synchronous test, so every attempt below is written
	// relative to one anchor, oldest first, exactly as the real ledger only ever grows forward in time.
	const double RealNow = FApp::GetCurrentTime();
	const double Window = static_cast<double>(UCrowdyGameModelSubsystem::InvokeBudgetWindowSeconds);

	// Fill well past the ring's capacity, all inside the window. The count saturates at the capacity rather than
	// growing, and nothing is lost that could still be inside the window and countable.
	const int32 Capacity = 4 * UCrowdyGameModelSubsystem::InvokeBudgetLimitPerWindow;
	for (int32 Index = 0; Index < Capacity + 50; ++Index)
	{
		// Spread across the last half-window so every one of them is still live, and strictly ascending.
		Model->RecordInvokeAttemptForTest(RealNow - (Window * 0.5) + (Index * 0.0001));
	}
	TestEqual(TEXT("a burst past the ring's capacity saturates at it rather than growing"),
		Model->GetRecentInvokeCount(), Capacity);

	// One more attempt, written at a time that is inside the window, must still be counted: the ring overwrote its
	// oldest entry rather than refusing the write.
	Model->RecordInvokeAttemptForTest(RealNow);
	TestEqual(TEXT("a further attempt still counts, because the ring dropped its oldest entry to take it"),
		Model->GetRecentInvokeCount(), Capacity);

	// A fresh ledger, driven across the window boundary: only what is still inside it counts.
	UCrowdyGameModelSubsystem* Fresh = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("second subsystem created"), Fresh))
	{
		return false;
	}
	Fresh->RecordInvokeAttemptForTest(RealNow - Window - 5.0); // long expired
	Fresh->RecordInvokeAttemptForTest(RealNow - Window - 0.1); // just expired
	Fresh->RecordInvokeAttemptForTest(RealNow - Window + 0.1); // just live
	Fresh->RecordInvokeAttemptForTest(RealNow);                // live
	TestEqual(TEXT("only the attempts still inside the sliding window are counted after a wrap-free run"),
		Fresh->GetRecentInvokeCount(), 2);

	return true;
}

// The per-entity backoff for an entity whose container has not resolved yet: a doubling delay on its own attempt
// count, clamped into [Min, Max], so an entity waiting on something outside this client is asked about less and
// less often rather than once per sweep forever. Mutating the doubling base or either clamp turns one of these
// exact values red without the others moving.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelPendingRetryDelayTest,
	"CrowdySDK.GameModel.PendingRetryDelay", CrowdyGovernorTestFlags)
bool FCrowdyGameModelPendingRetryDelayTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("the first attempt waits the floor"),
		UCrowdyGameModelSubsystem::ResolvePendingRetryDelaySeconds(0), 1.0f);
	TestEqual(TEXT("the second doubles"),
		UCrowdyGameModelSubsystem::ResolvePendingRetryDelaySeconds(1), 2.0f);
	TestEqual(TEXT("the third doubles again"),
		UCrowdyGameModelSubsystem::ResolvePendingRetryDelaySeconds(2), 4.0f);
	TestEqual(TEXT("the fifth is still under the ceiling"),
		UCrowdyGameModelSubsystem::ResolvePendingRetryDelaySeconds(4), 16.0f);
	TestEqual(TEXT("the sixth would double past the ceiling, so it is clamped to it"),
		UCrowdyGameModelSubsystem::ResolvePendingRetryDelaySeconds(5), 30.0f);
	TestEqual(TEXT("a far larger attempt count never grows past the ceiling either"),
		UCrowdyGameModelSubsystem::ResolvePendingRetryDelaySeconds(1000), 30.0f);

	// A nonsensical (negative) attempt count must not read as a shorter wait than the floor.
	TestEqual(TEXT("a negative attempt count still waits at least the floor"),
		UCrowdyGameModelSubsystem::ResolvePendingRetryDelaySeconds(-3), 1.0f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
