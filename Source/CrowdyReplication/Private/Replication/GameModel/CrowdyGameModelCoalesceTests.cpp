#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyEffects.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"

// UCrowdyEffects::EnqueueCoalescedInvoke and the merge window it opens: which applies are the SAME call for
// merging purposes, that N of them sum into one invoke's parameters, and that every caller who was offered a
// window - merged or not, flushed by timeout or drained by teardown - hears back exactly once. None of this
// needs a live server: it is proven by inspecting the shared JSON parameter object a window mutates in place,
// and by counting callbacks against calls made.
namespace
{
	constexpr EAutomationTestFlags CrowdyCoalesceTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Stand-in merge discriminators. The real ones are hashes of the effect asset, the level and the non-summed
	// overrides (UCrowdyEffects::BuildMergeDiscriminator); all the coalescer does with one is compare it, so any
	// distinct values exercise the same decision.
	constexpr uint64 DiscA = 0x1111111111111111ULL;
	constexpr uint64 DiscB = 0x2222222222222222ULL;
	constexpr uint64 DiscC = 0x3333333333333333ULL;
	constexpr uint64 DiscSum = 0x4444444444444444ULL;

	// EnqueueCoalescedInvoke requires GetWorld() != nullptr to consider a request mergeable at all, and a real
	// flush needs a real FTimerManager to fire a real timer against. A worldless NewObject (as the completion-
	// liveness suite uses) can never merge anything, so these tests need an actual, if minimal, UWorld.
	struct FCrowdyCoalesceTestWorld
	{
		UWorld* World = nullptr;

		FCrowdyCoalesceTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyCoalesceTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	// Every dispatch that actually reaches DispatchInvokeAttempt fails at ResolveApiContext (no game session
	// exists headlessly), logged at Error; whitelist whichever of the three fires first, exactly as the
	// completion-liveness suite does for the same reason. Named distinctly from that suite's own helper of the
	// same purpose so two anonymous-namespace definitions never collide when adaptive unity merges this
	// module's translation units.
	void AllowMissingApiContextForCoalesceTests(FAutomationTestBase& Test)
	{
		Test.AddExpectedError(TEXT("Game API endpoint is empty|No UCrowdyGameSession|No app-scoped game token"),
			EAutomationExpectedErrorFlags::Contains, 0);
	}

	// A free/data-container coalesce request naming AccumulateParam as a JSON number field. Params is handed
	// back so a test can hold the SAME shared object a merged window will later mutate in place.
	FCrowdyCoalesceRequest MakeRequest(const FString& ContainerId, const FString& FunctionName,
		const FString& AccumulateParam, double AccumulateValue, float WindowSeconds, uint64 Discriminator,
		TSharedPtr<FJsonObject>& OutParams)
	{
		OutParams = MakeShared<FJsonObject>();
		OutParams->SetNumberField(AccumulateParam, AccumulateValue);

		FCrowdyCoalesceRequest Request;
		Request.bEntityBound = false;
		Request.ContainerId = ContainerId;
		Request.FunctionName = FunctionName;
		Request.Params = OutParams;
		Request.AccumulateParam = AccumulateParam;
		Request.bAccumulateIsInteger = true;
		Request.WindowSeconds = WindowSeconds;
		Request.MergeDiscriminator = Discriminator;
		return Request;
	}

	// The same, bound to an entity rather than to a container id, so the target-binding rules are drivable.
	FCrowdyCoalesceRequest MakeEntityRequest(const FGuid& NetID, const FString& FunctionName,
		const FString& AccumulateParam, double AccumulateValue, float WindowSeconds, uint64 Discriminator,
		TSharedPtr<FJsonObject>& OutParams)
	{
		FCrowdyCoalesceRequest Request =
			MakeRequest(FString(), FunctionName, AccumulateParam, AccumulateValue, WindowSeconds, Discriminator, OutParams);
		Request.bEntityBound = true;
		Request.SelfNetID = NetID;
		return Request;
	}

	// Fire a timer this suite armed. A timer set while the manager has not been ticked this frame is queued as
	// Pending holding its time REMAINING, and only becomes active at the end of the following tick, with its expiry
	// rebased onto the clock that tick advanced. So the first tick activates the timer and the second crosses it,
	// and the frame counter has to move between them because the manager returns immediately from a second tick
	// inside one frame. A real world gets this for free by ticking every frame, which is also why a window's true
	// duration is its authored length plus up to one frame. Each step is generous enough to clear the longest delay
	// this suite arms without landing exactly on an expiry.
	void TickPastWindow(UWorld* World)
	{
		World->GetTimerManager().Tick(3.0f);
		++GFrameCounter;
		World->GetTimerManager().Tick(3.0f);
	}
}

// Two applies that agree on route, function, session, accumulated parameter and discriminator merge into one
// open window; a third that differs only in its discriminator opens a second window instead of joining the
// first. A request that cannot be merged at all (no window, or an accumulate field that is not a JSON number)
// is still accepted and dispatched on its own, and never opens or touches a window. Dropping any one field from
// FCrowdyCoalesceKey (or from its equality) would let the third request merge into the first; adding a field that
// varies per apply would stop the first two from merging.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCoalesceMergesMatchingAppliesTest,
	"CrowdySDK.GameModel.CoalesceMergesMatchingApplies", CrowdyCoalesceTestFlags)
bool FCrowdyGameModelCoalesceMergesMatchingAppliesTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContextForCoalesceTests(*this);

	FCrowdyCoalesceTestWorld Env;
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	if (!TestNotNull(TEXT("subsystem created against a real world"), Model))
	{
		return false;
	}

	int32 Notified = 0;
	auto CountingOnDone = [&Notified](FCrowdyInvokeResult) { ++Notified; };

	TSharedPtr<FJsonObject> ParamsA;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-1"), TEXT("ApplyDamage"), TEXT("amount"), 5, 5.0f, DiscA, ParamsA),
		CountingOnDone);
	TestEqual(TEXT("the first mergeable apply opens exactly one window"),
		Model->GetOpenCoalesceWindowCountForTest(), 1);

	TSharedPtr<FJsonObject> ParamsB;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-1"), TEXT("ApplyDamage"), TEXT("amount"), 3, 5.0f, DiscA, ParamsB),
		CountingOnDone);
	TestEqual(TEXT("an apply agreeing on every key field joins the open window rather than opening a second"),
		Model->GetOpenCoalesceWindowCountForTest(), 1);

	TSharedPtr<FJsonObject> ParamsC;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-1"), TEXT("ApplyDamage"), TEXT("amount"), 1, 5.0f, DiscB, ParamsC),
		CountingOnDone);
	TestEqual(TEXT("an apply differing only in its discriminator opens a distinct second window"),
		Model->GetOpenCoalesceWindowCountForTest(), 2);

	// A request with no merge window (WindowSeconds <= 0) is dispatched immediately and never opens a window.
	TSharedPtr<FJsonObject> ParamsD;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-1"), TEXT("ApplyDamage"), TEXT("amount"), 9, 0.0f, DiscA, ParamsD),
		CountingOnDone);
	TestEqual(TEXT("a non-positive window never opens a third window"),
		Model->GetOpenCoalesceWindowCountForTest(), 2);

	// A request whose accumulate field is not a JSON number (here a string) is also not mergeable, even though
	// it otherwise matches window A's key exactly, and is dispatched on its own rather than folded in.
	TSharedPtr<FJsonObject> NonNumericParams = MakeShared<FJsonObject>();
	NonNumericParams->SetStringField(TEXT("amount"), TEXT("5"));
	FCrowdyCoalesceRequest NonNumericRequest;
	NonNumericRequest.bEntityBound = false;
	NonNumericRequest.ContainerId = TEXT("cid-1");
	NonNumericRequest.FunctionName = TEXT("ApplyDamage");
	NonNumericRequest.Params = NonNumericParams;
	NonNumericRequest.AccumulateParam = TEXT("amount");
	NonNumericRequest.WindowSeconds = 5.0f;
	NonNumericRequest.MergeDiscriminator = DiscA;
	Model->EnqueueCoalescedInvoke(NonNumericRequest, CountingOnDone);
	TestEqual(TEXT("a non-numeric accumulate field is refused as mergeable and does not open a window"),
		Model->GetOpenCoalesceWindowCountForTest(), 2);

	// Both immediately-dispatched requests still ran their completion (missing API context, but notified).
	TestEqual(TEXT("both un-mergeable requests were still dispatched and notified on the spot"), Notified, 2);

	// Drain what is left open so this test does not leak a live timer into the next one.
	Model->FailPendingWorkForTest();
	return true;
}

// N applies inside one window sum into the ONE invoke's accumulated parameter, and every one of the N callers
// - not just the one whose apply happened to close the window - receives that single outcome. This flushes the
// window for real (a timer firing after its authored delay), the same code path a live sustained-fire effect
// takes, rather than forcing it through the full-waiters shortcut. Mutating DispatchCoalescedInvoke's summed
// write (dropping the FloorToDouble rounding, or writing AccumulatedValue instead of Summed) turns the exact
// total below red; mutating the fan-out to call only the LAST waiter (a plausible but wrong "optimization")
// turns the notified-count assertion red without the total moving.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCoalesceSummationAndFanOutTest,
	"CrowdySDK.GameModel.CoalesceSummationAndFanOut", CrowdyCoalesceTestFlags)
bool FCrowdyGameModelCoalesceSummationAndFanOutTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContextForCoalesceTests(*this);

	FCrowdyCoalesceTestWorld Env;
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	if (!TestNotNull(TEXT("subsystem created against a real world"), Model))
	{
		return false;
	}

	int32 Notified = 0;
	auto CountingOnDone = [&Notified](FCrowdyInvokeResult) { ++Notified; };

	// A short authored window (0.05s) so a single, generous Tick() below is unambiguously past it.
	TSharedPtr<FJsonObject> FirstParams;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-sum"), TEXT("ApplyDamage"), TEXT("amount"), 5, 0.05f, DiscSum, FirstParams),
		CountingOnDone);

	TSharedPtr<FJsonObject> SecondParams;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-sum"), TEXT("ApplyDamage"), TEXT("amount"), 3, 0.05f, DiscSum, SecondParams),
		CountingOnDone);

	TSharedPtr<FJsonObject> ThirdParams;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-sum"), TEXT("ApplyDamage"), TEXT("amount"), 10, 0.05f, DiscSum, ThirdParams),
		CountingOnDone);

	constexpr int32 CallsMade = 3;
	if (!TestEqual(TEXT("all three applies merged into a single open window"),
		Model->GetOpenCoalesceWindowCountForTest(), 1))
	{
		return false;
	}
	TestEqual(TEXT("nothing has been notified before the window closes"), Notified, 0);

	// Only the FIRST apply's Params object is kept as the window's carrier (see EnqueueCoalescedInvoke); the
	// summed value is written into it in place when the window flushes, so FirstParams sees it directly with
	// no need to inspect any private subsystem state.
	TestEqual(TEXT("the carried params object still holds only the first apply's value before flush"),
		FirstParams->GetNumberField(TEXT("amount")), 5.0);

	TickPastWindow(Env.World);

	TestEqual(TEXT("the window closed and sent its one merged invoke"),
		Model->GetOpenCoalesceWindowCountForTest(), 0);
	TestEqual(TEXT("every one of the three callers received an outcome, not just the one that closed the window"),
		Notified, CallsMade);
	TestEqual(TEXT("the three magnitudes summed exactly (5 + 3 + 10)"),
		FirstParams->GetNumberField(TEXT("amount")), 18.0);

	return true;
}

// Every caller waiting on an open window must still be told something when the world tears down mid-window,
// across every window that was open, not merely the caller who happens to be first in the map. Asserting the
// COUNT of outcomes against the count of calls (rather than "at least one arrived") is what catches a fan-out
// that quietly drops every waiter but the head of the list. A late enqueue after the shutdown latch is set must
// also still notify its caller rather than silently queuing into a window that can never close.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCoalesceOutcomeFanOutSurvivesTeardownTest,
	"CrowdySDK.GameModel.CoalesceOutcomeFanOutSurvivesTeardown", CrowdyCoalesceTestFlags)
bool FCrowdyGameModelCoalesceOutcomeFanOutSurvivesTeardownTest::RunTest(const FString& Parameters)
{
	FCrowdyCoalesceTestWorld Env;
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	if (!TestNotNull(TEXT("subsystem created against a real world"), Model))
	{
		return false;
	}

	int32 Notified = 0;
	TArray<FString> Errors;
	auto CountingOnDone = [&Notified, &Errors](FCrowdyInvokeResult Result)
	{
		++Notified;
		Errors.Add(Result.ErrorMessage);
	};

	// Four callers merged into one window, one caller alone in a second (a different discriminator): five calls
	// made across two open windows, so the fan-out has to reach every waiter in both, not just window one.
	TSharedPtr<FJsonObject> ParamsUnused;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Model->EnqueueCoalescedInvoke(
			MakeRequest(TEXT("cid-teardown"), TEXT("ApplyDamage"), TEXT("amount"), Index, 5.0f, DiscA, ParamsUnused),
			CountingOnDone);
	}
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-teardown"), TEXT("ApplyDamage"), TEXT("amount"), 99, 5.0f, DiscB, ParamsUnused),
		CountingOnDone);

	constexpr int32 CallsMade = 5;
	if (!TestEqual(TEXT("two distinct windows are open before teardown"),
		Model->GetOpenCoalesceWindowCountForTest(), 2))
	{
		return false;
	}
	TestEqual(TEXT("nothing has been notified yet"), Notified, 0);

	// The teardown drain: what Deinitialize does to every window still open when the world goes away.
	Model->FailPendingWorkForTest();

	TestEqual(TEXT("every open window was closed by the drain"), Model->GetOpenCoalesceWindowCountForTest(), 0);
	TestEqual(TEXT("the count of outcomes delivered equals the count of calls made, across both windows"),
		Notified, CallsMade);
	for (const FString& ErrorMessage : Errors)
	{
		TestTrue(TEXT("each drained outcome honestly reports the teardown rather than a fabricated success"),
			ErrorMessage.Contains(TEXT("torn down")));
	}

	// The shutdown latch (bShuttingDown) makes a LATER enqueue un-mergeable rather than silently queued into a
	// window that can now never close; its caller must still be told something.
	AllowMissingApiContextForCoalesceTests(*this);
	Notified = 0;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-teardown"), TEXT("ApplyDamage"), TEXT("amount"), 1, 5.0f, DiscC, ParamsUnused),
		CountingOnDone);
	TestEqual(TEXT("a post-shutdown enqueue still notifies its caller instead of vanishing into a dead window"),
		Notified, 1);
	TestEqual(TEXT("and opens no window, since nothing can ever close one again"),
		Model->GetOpenCoalesceWindowCountForTest(), 0);

	return true;
}

// The session a window is keyed on and sent against is the one RESOLVED at enqueue, not the caller's raw (often
// empty) Session Id. Two applies that both leave the session blank while the active session changes between them
// are two different calls, and must not sum into one. Reverting the key to hash Request.SessionId turns the
// second assertion red: both raw ids are empty, so both applies would land in one window and the whole summed
// magnitude would be sent against whichever session was active when the timer fired.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCoalesceKeysOnResolvedSessionTest,
	"CrowdySDK.GameModel.CoalesceKeysOnResolvedSession", CrowdyCoalesceTestFlags)
bool FCrowdyGameModelCoalesceKeysOnResolvedSessionTest::RunTest(const FString& Parameters)
{
	FCrowdyCoalesceTestWorld Env;
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	if (!TestNotNull(TEXT("subsystem created against a real world"), Model))
	{
		return false;
	}

	int32 Notified = 0;
	auto CountingOnDone = [&Notified](FCrowdyInvokeResult) { ++Notified; };

	// Two applies under one active session, both leaving Session Id blank: same resolved session, one window.
	Model->SetActiveSession(TEXT("session-one"));

	TSharedPtr<FJsonObject> FirstParams;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-session"), TEXT("ApplyDamage"), TEXT("amount"), 5, 5.0f, DiscA, FirstParams),
		CountingOnDone);
	TSharedPtr<FJsonObject> SecondParams;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-session"), TEXT("ApplyDamage"), TEXT("amount"), 5, 5.0f, DiscA, SecondParams),
		CountingOnDone);
	TestEqual(TEXT("two applies resolving to the same session share one window"),
		Model->GetOpenCoalesceWindowCountForTest(), 1);

	// The active session changes mid-window. A third apply, identical in every raw field to the first two, now
	// resolves somewhere else and must open its own window rather than adding to a sum bound for session one.
	Model->SetActiveSession(TEXT("session-two"));
	TSharedPtr<FJsonObject> ThirdParams;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-session"), TEXT("ApplyDamage"), TEXT("amount"), 5, 5.0f, DiscA, ThirdParams),
		CountingOnDone);
	TestEqual(TEXT("an apply made after the active session changed opens its own window"),
		Model->GetOpenCoalesceWindowCountForTest(), 2);

	// An explicit Session Id still wins over the active one, so naming session one again rejoins the FIRST window
	// rather than opening a third: the key follows the resolution rule, not the literal field.
	TSharedPtr<FJsonObject> FourthParams;
	FCrowdyCoalesceRequest Explicit =
		MakeRequest(TEXT("cid-session"), TEXT("ApplyDamage"), TEXT("amount"), 5, 5.0f, DiscA, FourthParams);
	Explicit.SessionId = TEXT("session-one");
	Model->EnqueueCoalescedInvoke(Explicit, CountingOnDone);
	TestEqual(TEXT("an explicit session id rejoins the window that resolved to the same session"),
		Model->GetOpenCoalesceWindowCountForTest(), 2);

	TestEqual(TEXT("nothing was dispatched while all four applies were being keyed"), Notified, 0);

	Model->FailPendingWorkForTest();
	TestEqual(TEXT("every one of the four callers was still told what happened"), Notified, 4);
	return true;
}

// A merge window is pinned to the exact entity binding it opened against. An entity that unbinds and binds again
// is a different life of that entity, and because the binding key is deterministic the SAME server container row
// serves both lives - so a window left over from the previous life must not flush its summed magnitude onto the
// new one. It is failed instead, and every waiter is told, because several applies' worth of effect is being
// dropped and a silent discard would look exactly like a successful hit that did nothing.
//
// Deleting the bind-epoch comparison in DispatchCoalescedInvoke turns the "reported the binding change" assertion
// red (the window would dispatch onto the new life instead). Removing BindEpoch from FCrowdyCoalesceKey turns the
// two-windows assertion red (the pre- and post-rebind applies would sum together).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCoalesceWindowPinnedToBindingTest,
	"CrowdySDK.GameModel.CoalesceWindowPinnedToBinding", CrowdyCoalesceTestFlags)
bool FCrowdyGameModelCoalesceWindowPinnedToBindingTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContextForCoalesceTests(*this);

	FCrowdyCoalesceTestWorld Env;
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	if (!TestNotNull(TEXT("subsystem created against a real world"), Model))
	{
		return false;
	}

	int32 Notified = 0;
	TArray<FString> Errors;
	auto CountingOnDone = [&Notified, &Errors](FCrowdyInvokeResult Result)
	{
		++Notified;
		Errors.Add(Result.ErrorMessage);
	};

	// One entity, bound to a container row a respawn would resolve to again.
	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainerForTest(NetID, TEXT("cid-victim"));

	TSharedPtr<FJsonObject> FirstParams;
	Model->EnqueueCoalescedInvoke(
		MakeEntityRequest(NetID, TEXT("ApplyDamage"), TEXT("amount"), 5, 0.05f, DiscA, FirstParams),
		CountingOnDone);
	TSharedPtr<FJsonObject> SecondParams;
	Model->EnqueueCoalescedInvoke(
		MakeEntityRequest(NetID, TEXT("ApplyDamage"), TEXT("amount"), 5, 0.05f, DiscA, SecondParams),
		CountingOnDone);
	if (!TestEqual(TEXT("two applies to the same live binding merged into one window"),
		Model->GetOpenCoalesceWindowCountForTest(), 1))
	{
		return false;
	}

	// The victim dies and respawns: the binding drops and is remade to the SAME deterministic container row.
	Model->UnbindEntityContainerForTest(NetID);
	Model->BindEntityContainerForTest(NetID, TEXT("cid-victim"));

	// An apply against the new life cannot join the window the previous life opened, even though the entity id,
	// the container row, the function and the discriminator are all identical.
	TSharedPtr<FJsonObject> ThirdParams;
	Model->EnqueueCoalescedInvoke(
		MakeEntityRequest(NetID, TEXT("ApplyDamage"), TEXT("amount"), 7, 0.05f, DiscA, ThirdParams),
		CountingOnDone);
	TestEqual(TEXT("an apply against the new life opens its own window rather than joining the old one"),
		Model->GetOpenCoalesceWindowCountForTest(), 2);

	TickPastWindow(Env.World);

	TestEqual(TEXT("both windows closed"), Model->GetOpenCoalesceWindowCountForTest(), 0);
	TestEqual(TEXT("all three callers were told an outcome"), Notified, 3);

	// The two applies from the previous life are told the target's binding changed, rather than being silently
	// dropped or landing on the new life.
	int32 BindingChangeReports = 0;
	for (const FString& ErrorMessage : Errors)
	{
		if (ErrorMessage.Contains(TEXT("binding changed")))
		{
			++BindingChangeReports;
		}
	}
	TestEqual(TEXT("the previous life's two merged applies both reported the binding change"),
		BindingChangeReports, 2);

	// The stale window's magnitude was never written into its parameters, so nothing could have been sent.
	TestEqual(TEXT("the stale window's summed magnitude was never marshalled"),
		FirstParams->GetNumberField(TEXT("amount")), 5.0);
	return true;
}

// An entity-bound apply with NOTHING bound cannot key a window at all: there is no resolved container to key it
// on, and parking it would let it resolve to whatever binding appeared later. It is dispatched on its own, which
// reports the missing binding immediately. Keying such an apply on the entity id alone turns the window-count
// assertion red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCoalesceRefusesUnboundEntityTest,
	"CrowdySDK.GameModel.CoalesceRefusesUnboundEntity", CrowdyCoalesceTestFlags)
bool FCrowdyGameModelCoalesceRefusesUnboundEntityTest::RunTest(const FString& Parameters)
{
	FCrowdyCoalesceTestWorld Env;
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	if (!TestNotNull(TEXT("subsystem created against a real world"), Model))
	{
		return false;
	}

	// InvokeAndApply logs this at Warning, not Error, but the dispatch that follows still reports it to the caller.
	int32 Notified = 0;
	FString LastError;
	auto CountingOnDone = [&Notified, &LastError](FCrowdyInvokeResult Result)
	{
		++Notified;
		LastError = Result.ErrorMessage;
	};

	TSharedPtr<FJsonObject> Params;
	Model->EnqueueCoalescedInvoke(
		MakeEntityRequest(FGuid::NewGuid(), TEXT("ApplyDamage"), TEXT("amount"), 5, 5.0f, DiscA, Params),
		CountingOnDone);

	TestEqual(TEXT("an apply against an unbound entity opens no window"),
		Model->GetOpenCoalesceWindowCountForTest(), 0);
	TestEqual(TEXT("and its caller is told immediately"), Notified, 1);
	TestTrue(TEXT("the outcome names the missing binding rather than a merge failure"),
		LastError.Contains(TEXT("no container bound")));
	return true;
}

// An int magnitude is rounded PER APPLY, so a merged window delivers exactly what the same applies would have
// delivered one by one. Rounding only the sum made the delivered total depend on fire rate: three applies of 12.5
// sent individually deliver 13 each (39 in total), but summed first and rounded once they deliver 38.
//
// This needs fractional halves to see: whole-number magnitudes agree under either rule, which is why a naive test
// of this passes without the fix. Deleting the per-apply rounding in EnqueueCoalescedInvoke turns the merged
// total red (38 instead of 39); deleting it turns the single-apply assertion red too, because an unmerged apply
// would then still carry 12.5.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCoalesceIntegerRoundingMatchesUnmergedTest,
	"CrowdySDK.GameModel.CoalesceIntegerRoundingMatchesUnmerged", CrowdyCoalesceTestFlags)
bool FCrowdyGameModelCoalesceIntegerRoundingMatchesUnmergedTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContextForCoalesceTests(*this);

	FCrowdyCoalesceTestWorld Env;
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	if (!TestNotNull(TEXT("subsystem created against a real world"), Model))
	{
		return false;
	}

	auto Ignore = [](FCrowdyInvokeResult) {};

	// One apply of 12.5 on its own (no merge window at all): what the server is asked to apply is 13, the whole
	// number the magnitude was declared as, not the fraction.
	TSharedPtr<FJsonObject> LoneParams;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-round"), TEXT("ApplyDamage"), TEXT("amount"), 12.5, 0.0f, DiscA, LoneParams), Ignore);
	TestEqual(TEXT("a single unmerged apply of 12.5 is sent as 13"),
		LoneParams->GetNumberField(TEXT("amount")), 13.0);

	// The same three applies merged. Each rounds on the way in, so the window carries 13 + 13 + 13.
	TSharedPtr<FJsonObject> FirstParams;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-round"), TEXT("ApplyDamage"), TEXT("amount"), 12.5, 0.05f, DiscB, FirstParams), Ignore);
	TSharedPtr<FJsonObject> SecondParams;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-round"), TEXT("ApplyDamage"), TEXT("amount"), 12.5, 0.05f, DiscB, SecondParams), Ignore);
	TSharedPtr<FJsonObject> ThirdParams;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-round"), TEXT("ApplyDamage"), TEXT("amount"), 12.5, 0.05f, DiscB, ThirdParams), Ignore);
	if (!TestEqual(TEXT("the three fractional applies merged into one window"),
		Model->GetOpenCoalesceWindowCountForTest(), 1))
	{
		return false;
	}

	TickPastWindow(Env.World);

	TestEqual(TEXT("three merged applies of 12.5 deliver the same 39 that three unmerged applies would"),
		FirstParams->GetNumberField(TEXT("amount")), 39.0);

	// A float magnitude is untouched by any of this: only a parameter declared as an int is rounded.
	TSharedPtr<FJsonObject> FloatParams;
	FCrowdyCoalesceRequest FloatRequest =
		MakeRequest(TEXT("cid-round"), TEXT("Heal"), TEXT("amount"), 12.5, 0.0f, DiscC, FloatParams);
	FloatRequest.bAccumulateIsInteger = false;
	Model->EnqueueCoalescedInvoke(FloatRequest, Ignore);
	TestEqual(TEXT("a float magnitude keeps its fraction"),
		FloatParams->GetNumberField(TEXT("amount")), 12.5);
	return true;
}

// Past the open-window bound the coalescer closes its OLDEST window early to make room, rather than switching
// merging off and sending every further apply on its own. That bound is reached exactly when the allowance is
// tightest, so giving up on merging there spends it fastest.
//
// The discriminator between the two behaviours is WHICH caller hears back: closing the oldest notifies the caller
// that has been waiting longest and leaves the newest apply merging in a window, while the old behaviour notified
// the newest apply immediately and left every existing window untouched. Restoring the immediate dispatch turns
// both assertions red at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelCoalesceKeepsMergingAtCapacityTest,
	"CrowdySDK.GameModel.CoalesceKeepsMergingAtCapacity", CrowdyCoalesceTestFlags)
bool FCrowdyGameModelCoalesceKeepsMergingAtCapacityTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContextForCoalesceTests(*this);

	FCrowdyCoalesceTestWorld Env;
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	if (!TestNotNull(TEXT("subsystem created against a real world"), Model))
	{
		return false;
	}

	const int32 MaxOpenWindows = UCrowdyGameModelSubsystem::MaxOpenCoalesceWindows;

	TArray<int32> NotifiedIndices;
	auto MakeOnDone = [&NotifiedIndices](int32 Index)
	{
		return [&NotifiedIndices, Index](FCrowdyInvokeResult) { NotifiedIndices.Add(Index); };
	};

	// One window per distinct target, because a merge key is per target: this is also what bounds how many
	// separate containers one client can have effects in flight against at once.
	TArray<TSharedPtr<FJsonObject>> AllParams;
	AllParams.SetNum(MaxOpenWindows + 1);
	for (int32 Index = 0; Index < MaxOpenWindows; ++Index)
	{
		Model->EnqueueCoalescedInvoke(
			MakeRequest(FString::Printf(TEXT("cid-%d"), Index), TEXT("ApplyDamage"), TEXT("amount"), 1, 5.0f,
				DiscA, AllParams[Index]),
			MakeOnDone(Index));
	}
	if (!TestEqual(TEXT("the coalescer filled to its open-window bound"),
		Model->GetOpenCoalesceWindowCountForTest(), MaxOpenWindows))
	{
		return false;
	}
	TestEqual(TEXT("nothing was dispatched while filling"), NotifiedIndices.Num(), 0);

	// One more distinct target, with the coalescer full.
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-overflow"), TEXT("ApplyDamage"), TEXT("amount"), 1, 5.0f, DiscA,
			AllParams[MaxOpenWindows]),
		MakeOnDone(MaxOpenWindows));

	TestEqual(TEXT("still exactly the bound: one window closed to make room for the new one"),
		Model->GetOpenCoalesceWindowCountForTest(), MaxOpenWindows);
	if (TestEqual(TEXT("exactly one caller heard back"), NotifiedIndices.Num(), 1))
	{
		TestEqual(TEXT("it is the OLDEST window's caller that was flushed, not the newest apply"),
			NotifiedIndices[0], 0);
	}

	// The newest apply is still merging, so a second apply to that same new target joins it rather than costing
	// another call: that is the whole point of closing a window instead of giving up on merging.
	TSharedPtr<FJsonObject> SecondToOverflow;
	Model->EnqueueCoalescedInvoke(
		MakeRequest(TEXT("cid-overflow"), TEXT("ApplyDamage"), TEXT("amount"), 1, 5.0f, DiscA, SecondToOverflow),
		MakeOnDone(MaxOpenWindows + 1));
	TestEqual(TEXT("a repeat apply to the new target merges rather than opening or flushing anything"),
		Model->GetOpenCoalesceWindowCountForTest(), MaxOpenWindows);
	TestEqual(TEXT("and cost no further dispatch"), NotifiedIndices.Num(), 1);

	Model->FailPendingWorkForTest();
	return true;
}

// The merge discriminator is what makes two applies of the same effect to the same target "the same call". It is a
// hash now rather than a built string, so what has to be re-proven is that it still separates exactly what it
// separated before: a different asset, a different level, a different Source, a different override VALUE. And that
// it still ignores exactly what it ignored: the order a caller's override map happens to enumerate in, and the
// value of the parameter being summed (which is the one thing applies are allowed to differ in).
//
// Dropping Level from the mix turns the level case red; dropping the accumulate-parameter exclusion turns the
// summed-parameter case red, and would stop every autofire apply from ever merging with the one before it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelMergeDiscriminatorSeparatesTest,
	"CrowdySDK.GameModel.MergeDiscriminatorSeparates", CrowdyCoalesceTestFlags)
bool FCrowdyGameModelMergeDiscriminatorSeparatesTest::RunTest(const FString& Parameters)
{
	UCrowdyEffect* Effect = NewObject<UCrowdyEffect>(GetTransientPackage());
	UCrowdyEffect* Other = NewObject<UCrowdyEffect>(GetTransientPackage());
	if (!TestNotNull(TEXT("effect created"), Effect) || !TestNotNull(TEXT("second effect created"), Other))
	{
		return false;
	}

	const FString Accumulate = TEXT("amount");

	TMap<FName, FString> Base;
	Base.Add(FName(TEXT("amount")), TEXT("5"));
	Base.Add(FName(TEXT("damage_type")), TEXT("\"fire\""));
	const uint64 Baseline = UCrowdyEffects::BuildMergeDiscriminator(Effect, Base, 1.0f, TEXT("src-a"), Accumulate);

	// Compared with TestTrue on an explicit ==, because the automation helpers have no unambiguous overload for an
	// unsigned 64-bit value and picking one by conversion would compare something other than these bits.

	// The summed parameter's own value is what applies are allowed to differ in, so it must not separate them.
	TMap<FName, FString> DifferentAmount;
	DifferentAmount.Add(FName(TEXT("damage_type")), TEXT("\"fire\""));
	DifferentAmount.Add(FName(TEXT("amount")), TEXT("9"));
	TestTrue(TEXT("a different value for the summed parameter is the same call"),
		UCrowdyEffects::BuildMergeDiscriminator(Effect, DifferentAmount, 1.0f, TEXT("src-a"), Accumulate) == Baseline);

	// Everything else about the parameters does separate them.
	TMap<FName, FString> DifferentType;
	DifferentType.Add(FName(TEXT("amount")), TEXT("5"));
	DifferentType.Add(FName(TEXT("damage_type")), TEXT("\"ice\""));
	TestFalse(TEXT("a different value for any other override is a different call"),
		UCrowdyEffects::BuildMergeDiscriminator(Effect, DifferentType, 1.0f, TEXT("src-a"), Accumulate) == Baseline);

	TestFalse(TEXT("a different level is a different call, because a curve would sample differently"),
		UCrowdyEffects::BuildMergeDiscriminator(Effect, Base, 2.0f, TEXT("src-a"), Accumulate) == Baseline);

	TestFalse(TEXT("a different Source is a different call, so one attacker's hit is never credited to another"),
		UCrowdyEffects::BuildMergeDiscriminator(Effect, Base, 1.0f, TEXT("src-b"), Accumulate) == Baseline);

	TestFalse(TEXT("a different effect asset is a different call, even with identical parameters"),
		UCrowdyEffects::BuildMergeDiscriminator(Other, Base, 1.0f, TEXT("src-a"), Accumulate) == Baseline);

	// An override the caller supplies that the baseline did not is also a different call: the fold is a sum of
	// per-entry hashes, so an added entry has to move it.
	TMap<FName, FString> Extra = Base;
	Extra.Add(FName(TEXT("crit")), TEXT("true"));
	TestFalse(TEXT("an extra override is a different call"),
		UCrowdyEffects::BuildMergeDiscriminator(Effect, Extra, 1.0f, TEXT("src-a"), Accumulate) == Baseline);

	// And with no summed parameter named, every override discriminates, including one that happens to be called
	// "amount": nothing is excluded when nothing is being summed.
	TestFalse(TEXT("with no accumulate parameter named, the amount override discriminates like any other"),
		UCrowdyEffects::BuildMergeDiscriminator(Effect, DifferentAmount, 1.0f, TEXT("src-a"), FString())
			== UCrowdyEffects::BuildMergeDiscriminator(Effect, Base, 1.0f, TEXT("src-a"), FString()));

	return true;
}

// The re-bind sweep for entities that have not resolved a container yet. It lives in this suite rather than beside
// the governor's pure functions because it needs the same real world and real timer manager the merge windows do,
// and a second copy of that fixture in another translation unit of this module would collide with this one the
// moment adaptive unity merged them.
//
// Three properties, each with its own failure it prevents. A burst of notifications produces ONE sweep, not one
// per notification (calling RetryPendingModelEntities straight from the notification path turns the first
// assertion red). One sweep re-drives a BOUNDED number of entities, since each is a network round trip and a busy
// world holds hundreds (removing the per-sweep bound turns the second red). And an entity already asked about
// waits out its own backoff instead of being asked again on the very next sweep (removing the backoff check turns
// the last two red: the same sixteen would be re-driven and no new entity would ever be reached).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelPendingSweepIsRateLimitedTest,
	"CrowdySDK.GameModel.PendingSweepIsRateLimited", CrowdyCoalesceTestFlags)
bool FCrowdyGameModelPendingSweepIsRateLimitedTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContextForCoalesceTests(*this);

	FCrowdyCoalesceTestWorld Env;
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	if (!TestNotNull(TEXT("subsystem created against a real world"), Model))
	{
		return false;
	}

	// More pending entities than one sweep may re-drive, so the bound is observable.
	constexpr int32 PendingCount = 40;
	const int32 PerSweep = UCrowdyGameModelSubsystem::MaxPendingResolvesPerSweep;
	TArray<FGuid> Pending;
	Pending.Reserve(PendingCount);
	for (int32 Index = 0; Index < PendingCount; ++Index)
	{
		const FGuid NetID = FGuid::NewGuid();
		Pending.Add(NetID);
		Model->AddPendingModelEntityForTest(NetID, TEXT("PlayerStats"));
	}

	// A storm of model-changed notifications for containers this client does not hold. Each one used to run a full
	// sweep on the spot.
	for (int32 Index = 0; Index < 50; ++Index)
	{
		Model->RequestPendingModelEntitySweepForTest();
	}
	TestEqual(TEXT("fifty notifications ran no sweep at all yet, only armed one"),
		Model->GetPendingSweepCountForTest(), 0);

	TickPastWindow(Env.World);

	TestEqual(TEXT("the fifty notifications produced exactly one sweep"),
		Model->GetPendingSweepCountForTest(), 1);
	TestEqual(TEXT("that sweep re-drove only its per-sweep bound of entities, not all forty"),
		Model->GetPendingRetryAttemptedCountForTest(), PerSweep);

	// A second sweep must move on to entities it has not asked about, because the ones it just asked about are
	// waiting out their backoff.
	Model->RetryPendingModelEntitiesForTest();
	TestEqual(TEXT("a second sweep reached a further bound's worth of entities"),
		Model->GetPendingRetryAttemptedCountForTest(), PerSweep * 2);

	int32 MostAttemptsForOneEntity = 0;
	int32 TotalAttempts = 0;
	for (const FGuid& NetID : Pending)
	{
		const int32 Attempts = Model->GetPendingRetryAttemptsForTest(NetID);
		MostAttemptsForOneEntity = FMath::Max(MostAttemptsForOneEntity, Attempts);
		TotalAttempts += Attempts;
	}
	TestEqual(TEXT("no entity was asked about twice across the two sweeps"), MostAttemptsForOneEntity, 1);
	TestEqual(TEXT("and the two sweeps together cost exactly two bounds' worth of round trips"),
		TotalAttempts, PerSweep * 2);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
