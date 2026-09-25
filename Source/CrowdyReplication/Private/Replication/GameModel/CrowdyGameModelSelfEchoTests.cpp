#include "CrowdyCppClient.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"

// A self-invoke marks its container when each attempt is sent and unmarks it when the attempt does not commit; each
// echo takes the oldest live mark, and a mark still unclaimed at its own expiry is counted and dropped.
namespace CrowdySelfEchoTestSupport
{
	constexpr EAutomationTestFlags SelfEchoTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const SelfEchoTestEndpoint = TEXT("https://game.test");

	// A committed invoke that wrote one key of its own container.
	const TCHAR* const SelfEchoSuccessBody = TEXT(
		"{\"data\":{\"gameModelInvoke\":{\"success\":true,\"returnValueJson\":\"1\",\"errorMessage\":\"\","
		"\"mutationsApplied\":[{\"key\":\"hp\",\"oldValueJson\":\"1\",\"newValueJson\":\"2\"}]}}}");

	// A committed invoke that wrote only another container, as a source.<attr> write does.
	const TCHAR* const SelfEchoOtherWriteBody = TEXT(
		"{\"data\":{\"gameModelInvoke\":{\"success\":true,\"returnValueJson\":\"1\",\"errorMessage\":\"\","
		"\"mutationsApplied\":[{\"containerId\":\"c-other\",\"key\":\"hp\",\"oldValueJson\":\"1\",\"newValueJson\":\"2\"}]}}}");

	// A committed invoke that wrote nothing, as a read-only function does.
	const TCHAR* const SelfEchoNoWriteBody =
		TEXT("{\"data\":{\"gameModelInvoke\":{\"success\":true,\"returnValueJson\":\"1\",\"errorMessage\":\"\",\"mutationsApplied\":[]}}}");

	const TCHAR* const SelfEchoInBandFaultBody = TEXT(
		"{\"data\":{\"gameModelInvoke\":{\"success\":false,\"returnValueJson\":null,\"errorMessage\":\"no\",\"mutationsApplied\":[],"
		"\"fault\":{\"code\":\"USER_CODE_ERROR\",\"blame\":\"AUTHOR\",\"retryable\":false}}}}");

	FString SelfEchoRefusalBody(const TCHAR* Code)
	{
		return FString::Printf(
			TEXT("{\"errors\":[{\"message\":\"Refused.\",\"extensions\":{\"code\":\"%s\",\"blame\":\"PLATFORM\",\"retryable\":true,\"retryAfterMs\":0}}],\"data\":null}"),
			Code);
	}

	// Sets FApp's clock for the scope and puts the previous reading back.
	struct FSelfEchoClockScope
	{
		double Previous = 0.0;

		FSelfEchoClockScope() : Previous(FApp::GetCurrentTime()) {}
		~FSelfEchoClockScope() { FApp::SetCurrentTime(Previous); }

		void Set(double Seconds) const { FApp::SetCurrentTime(Seconds); }
	};

	// A real world, so the subsystem has a timer manager for a retry or a merge window.
	struct FSelfEchoTestWorld
	{
		UWorld* World = nullptr;

		FSelfEchoTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FSelfEchoTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		// The first tick activates a timer armed this frame, the second crosses it.
		void FireTimers() const
		{
			World->GetTimerManager().Tick(6.0f);
			++GFrameCounter;
			World->GetTimerManager().Tick(6.0f);
		}
	};

	UCrowdyGameModelSubsystem* MakeSelfEchoLiveModel(UWorld* World)
	{
		UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(World);
		Model->BeginWorldSessionForTest();
		Model->SetApiContextForTest(SelfEchoTestEndpoint, TEXT("test-token"), 42);
		return Model;
	}

	struct FSelfEchoOutcome
	{
		int32 Delivered = 0;
		FCrowdyInvokeResult Last;
	};

	TFunction<void(FCrowdyInvokeResult)> CountInto(FSelfEchoOutcome& Out)
	{
		return [&Out](FCrowdyInvokeResult Result)
		{
			++Out.Delivered;
			Out.Last = MoveTemp(Result);
		};
	}

	FCrowdyModelChangeHint SelfEchoHint(const FString& ContainerId)
	{
		FCrowdyModelChangeHint Hint;
		Hint.ContainerId = ContainerId;
		return Hint;
	}
}

// Each mark expires on its own clock, so a mark late in a burst outlives an early one; a hint takes the oldest live
// mark; every expired mark is counted exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoPerMarkExpiryTest,
	"CrowdySDK.GameModel.SelfEcho.PerMarkExpiry", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoPerMarkExpiryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();
	const FSelfEchoClockScope Clock;

	Clock.Set(1000.0);
	Model->MarkSelfActed(TEXT("c1"));
	Clock.Set(1002.5);
	Model->MarkSelfActed(TEXT("c1"));
	Clock.Set(1003.1);
	TestTrue(TEXT("the late mark outlives the early one, so its echo is dropped"), Model->ConsumeSelfEcho(TEXT("c1")));
	TestEqual(TEXT("the early mark alone expired"), Stats.SelfEchoMarksExpired, 1);
	TestFalse(TEXT("and no mark is left"), Model->ConsumeSelfEcho(TEXT("c1")));

	Clock.Set(2000.0);
	Model->MarkSelfActed(TEXT("c2"));
	Model->MarkSelfActed(TEXT("c2"));
	Clock.Set(2001.0);
	Model->MarkSelfActed(TEXT("c2"));
	Clock.Set(2003.0);
	TestTrue(TEXT("a mark ends on its last second, and the later one still drops"), Model->ConsumeSelfEcho(TEXT("c2")));
	TestEqual(TEXT("each expired mark is counted"), Stats.SelfEchoMarksExpired, 3);
	Clock.Set(2003.5);
	TestFalse(TEXT("nothing is left"), Model->ConsumeSelfEcho(TEXT("c2")));
	TestEqual(TEXT("and nothing is counted twice"), Stats.SelfEchoMarksExpired, 3);

	Clock.Set(3000.0);
	Model->MarkSelfActed(TEXT("c3"));
	Clock.Set(3002.0);
	Model->MarkSelfActed(TEXT("c3"));
	TestTrue(TEXT("the first hint takes a mark"), Model->ConsumeSelfEcho(TEXT("c3")));
	Clock.Set(3003.5);
	TestTrue(TEXT("it took the oldest, so the newer one is still live"), Model->ConsumeSelfEcho(TEXT("c3")));
	TestEqual(TEXT("nothing expired"), Stats.SelfEchoMarksExpired, 3);

	TArray<FString> Lines;
	Model->DescribeNetStats(Lines);
	TestTrue(TEXT("the self-echo line prints the expired marks"), Lines.Contains(
		TEXT("[GameModel] self-echo: marked 7 dropped 4 hints-after-self-not-dropped 0 marks expired 3 backstop pulls 0")));

	Clock.Set(4000.0);
	Model->MarkSelfActed(TEXT("c4"));
	Clock.Set(4004.0);
	TestFalse(TEXT("a hint for an unmarked container"), Model->ConsumeSelfEcho(TEXT("c5")));
	TestEqual(TEXT("prunes no other container"), Stats.SelfEchoMarksExpired, 3);
	TestFalse(TEXT("a hint for the expired container"), Model->ConsumeSelfEcho(TEXT("c4")));
	TestEqual(TEXT("prunes its own"), Stats.SelfEchoMarksExpired, 4);

	Model->ResetNetStats();
	TestEqual(TEXT("a reset clears the expired marks"), Stats.SelfEchoMarksExpired, 0);
	return true;
}

// Every dropped hint is followed by a pull of its container one window later, one per container however many drops,
// so a change hidden behind a mark whose own echo never came is seen within the window.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoBackstopTest,
	"CrowdySDK.GameModel.SelfEcho.BackstopPullsAfterDrop", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoBackstopTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;
	// The queued pulls drain on a later tick, with no client host to send them.
	AddExpectedMessagePlain(TEXT("No Game API client host"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 0);

	FSelfEchoTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();
	const FGuid First = FGuid::NewGuid();
	const FGuid Second = FGuid::NewGuid();
	Model->BindEntityContainer(First, TEXT("c1"));
	Model->BindEntityContainer(Second, TEXT("c2"));

	// Each step is its own frame, since the timer manager ticks once per frame; the first activates the armed timer.
	FTimerManager& Timers = Env.World->GetTimerManager();
	auto Advance = [&Timers](float Seconds)
	{
		++GFrameCounter;
		Timers.Tick(Seconds);
	};

	Model->MarkSelfActed(TEXT("c1"));
	Model->HandleModelChangeHintForTest(SelfEchoHint(TEXT("c1")));
	TestEqual(TEXT("nothing is pulled at once"), Model->GetPendingRefreshPullCountForTest(), 0);
	Advance(0.001f);
	Advance(2.0f);
	TestEqual(TEXT("no pull 2 s after the first drop"), Stats.SelfEchoBackstopPulls, 0);

	// Later drops in the round do not push it back.
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Model->MarkSelfActed(TEXT("c1"));
		Model->HandleModelChangeHintForTest(SelfEchoHint(TEXT("c1")));
	}
	Model->MarkSelfActed(TEXT("c2"));
	Model->HandleModelChangeHintForTest(SelfEchoHint(TEXT("c2")));
	TestEqual(TEXT("every hint was dropped"), Stats.SelfEchoesDropped, 4);
	Advance(1.5f);
	TestEqual(TEXT("one pull per container, 3 s after the round's first drop"), Stats.SelfEchoBackstopPulls, 2);
	FGuid Target;
	TestTrue(TEXT("the first container is queued"), Model->TryGetPendingRefreshPullTargetForTest(TEXT("c1"), Target));
	TestTrue(TEXT("and the second"), Model->TryGetPendingRefreshPullTargetForTest(TEXT("c2"), Target));

	Model->MarkSelfActed(TEXT("c1"));
	Model->HandleModelChangeHintForTest(SelfEchoHint(TEXT("c1")));
	Env.FireTimers();
	TestEqual(TEXT("a drop after the pull arms the next one"), Stats.SelfEchoBackstopPulls, 3);

	Model->MarkSelfActed(TEXT("c1"));
	Model->HandleModelChangeHintForTest(SelfEchoHint(TEXT("c1")));
	Model->FailPendingWorkForTest();
	Env.FireTimers();
	TestEqual(TEXT("teardown cancels a pending backstop"), Stats.SelfEchoBackstopPulls, 3);

	Model->ResetNetStats();
	TestEqual(TEXT("a reset clears the backstop pulls"), Stats.SelfEchoBackstopPulls, 0);
	return true;
}

// A row this client holds twice is not marked: the invoke's writes reach one holder, so the echo must pull for the other.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoSharedRowTest,
	"CrowdySDK.GameModel.SelfEcho.SharedRowNotMarked", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoSharedRowTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;
	AddExpectedMessagePlain(TEXT("a container row is meant to have one local holder"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 2);

	FSelfEchoTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);
	const FGuid First = FGuid::NewGuid();
	Model->BindEntityContainer(First, TEXT("c1"));
	Model->BindEntityContainer(FGuid::NewGuid(), TEXT("c1"));

	FSelfEchoOutcome Outcome;
	Model->InvokeAndApply(First, TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(Outcome));
	TestEqual(TEXT("nothing marked"), Model->GetNetStats().SelfInvokesMarked, 0);
	TestFalse(TEXT("so the echo pulls"), Model->ConsumeSelfEcho(TEXT("c1")));
	ClientHost.PollUntil([&Outcome] { return Outcome.Delivered > 0; });
	TestTrue(TEXT("the invoke committed"), Outcome.Last.bTransportOk && Outcome.Last.bSuccess);

	// A second holder that binds after the mark still hears the echo.
	Model->BindEntityContainer(FGuid::NewGuid(), TEXT("c2"));
	TestTrue(TEXT("one holder is marked"), Model->MarkSelfActed(TEXT("c2")) > 0.0);
	Model->BindEntityContainer(FGuid::NewGuid(), TEXT("c2"));
	Model->HandleModelChangeHintForTest(SelfEchoHint(TEXT("c2")));
	TestEqual(TEXT("the echo is not dropped once the row has two holders"), Model->GetNetStats().SelfEchoesDropped, 0);
	FGuid Target;
	TestTrue(TEXT("so it pulls"), Model->TryGetPendingRefreshPullTargetForTest(TEXT("c2"), Target));
	return true;
}

// An uncommitted attempt gives back its own mark, not a newer attempt's; one that ends past its own expiry does
// nothing, even when a hint took its mark.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoOwnMarkTest,
	"CrowdySDK.GameModel.SelfEcho.UnmarkTakesOwnMark", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoOwnMarkTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;

	{
		FSelfEchoTestWorld Env;
		UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
		FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);
		ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, SelfEchoSuccessBody),
			TPair<int32, FString>(500, TEXT("")) });
		const FSelfEchoClockScope Clock;

		// The late attempt fails, so its own mark and the oldest mark are different ones.
		FSelfEchoOutcome Early;
		FSelfEchoOutcome Late;
		Clock.Set(1000.0);
		Model->InvokeOnContainer(TEXT("c1"), TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(Early));
		Clock.Set(1001.0);
		Model->InvokeOnContainer(TEXT("c1"), TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(Late));
		ClientHost.PollUntil([&Early, &Late] { return Early.Delivered > 0 && Late.Delivered > 0; });
		TestTrue(TEXT("the early attempt committed"), Early.Last.bTransportOk && Early.Last.bSuccess);
		TestFalse(TEXT("the late one failed"), Late.Last.bTransportOk);
		Clock.Set(1003.5);
		TestFalse(TEXT("the late attempt removed its own mark, so only the early one's was left to expire"),
			Model->ConsumeSelfEcho(TEXT("c1")));
		TestEqual(TEXT("and the early mark expired"), Model->GetNetStats().SelfEchoMarksExpired, 1);
	}

	FSelfEchoTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);
	ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(500, TEXT("")) });
	const FSelfEchoClockScope Clock;
	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("c1"));

	FSelfEchoOutcome Slow;
	Clock.Set(1000.0);
	Model->InvokeAndApply(NetID, TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(Slow));
	Model->HandleModelChangeHintForTest(SelfEchoHint(TEXT("c1")));
	Clock.Set(1004.0);
	ClientHost.PollUntil([&Slow] { return Slow.Delivered > 0; });
	TestFalse(TEXT("the slow attempt failed"), Slow.Last.bTransportOk);
	TestEqual(TEXT("past its expiry it pulls nothing"), Model->GetPendingRefreshPullCountForTest(), 0);
	return true;
}

// An attempt answered before it was ever sent is neither marked nor counted as dispatched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoUnsentTest,
	"CrowdySDK.GameModel.SelfEcho.UnsentAttemptNotMarked", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoUnsentTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;

	FSelfEchoTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);
	ClientHost.Client->Close();

	FSelfEchoOutcome Outcome;
	Model->InvokeOnContainer(TEXT("c1"), TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(Outcome));
	TestTrue(TEXT("answered without being sent"), Outcome.Delivered == 1 && !Outcome.Last.bTransportOk);
	TestEqual(TEXT("not dispatched"), Model->GetNetStats().InvokesDispatched, 0);
	TestEqual(TEXT("not marked"), Model->GetNetStats().SelfInvokesMarked, 0);
	TestFalse(TEXT("so nothing is left to drop"), Model->ConsumeSelfEcho(TEXT("c1")));
	return true;
}

// An uncommitted attempt whose mark an echo already took pulls the container, since that echo may have been for a
// change that committed unseen; one that still finds a mark pulls nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoLostResponseTest,
	"CrowdySDK.GameModel.SelfEcho.LostResponsePulls", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoLostResponseTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;

	for (const bool bEchoFirst : { true, false })
	{
		const TCHAR* Name = bEchoFirst ? TEXT("echo taken") : TEXT("mark still held");
		FSelfEchoTestWorld Env;
		UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
		FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);
		ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(500, TEXT("")) });
		const FGuid NetID = FGuid::NewGuid();
		Model->BindEntityContainer(NetID, TEXT("c1"));

		FSelfEchoOutcome Outcome;
		Model->InvokeAndApply(NetID, TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(Outcome));
		if (bEchoFirst)
		{
			Model->HandleModelChangeHintForTest(SelfEchoHint(TEXT("c1")));
		}
		TestEqual(FString::Printf(TEXT("%s: nothing queued before the response"), Name),
			Model->GetPendingRefreshPullCountForTest(), 0);

		ClientHost.PollUntil([&Outcome] { return Outcome.Delivered > 0; });
		TestFalse(FString::Printf(TEXT("%s: the attempt failed"), Name), Outcome.Last.bTransportOk);
		FGuid Target;
		TestEqual(FString::Printf(TEXT("%s: a pull is queued only when the echo was taken"), Name),
			Model->TryGetPendingRefreshPullTargetForTest(TEXT("c1"), Target), bEchoFirst);
		TestTrue(FString::Printf(TEXT("%s: for the entity bound to the row"), Name), Target == (bEchoFirst ? NetID : FGuid()));
	}

	// A watched data container has no entity to queue for, so it is pulled directly, as its notification would be.
	FSelfEchoTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);
	ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, SelfEchoSuccessBody),
		TPair<int32, FString>(500, TEXT("")) });
	FSelfEchoOutcome Watched;
	Model->InvokeOnContainer(TEXT("d1"), TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(Watched));
	ClientHost.PollUntil([&Watched] { return Watched.Delivered > 0; });
	Model->ConsumeSelfEcho(TEXT("d1"));

	FSelfEchoOutcome Lost;
	Model->InvokeOnContainer(TEXT("d1"), TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(Lost));
	Model->HandleModelChangeHintForTest(SelfEchoHint(TEXT("d1")));
	TestEqual(TEXT("data: the echo was dropped, so nothing is pulled yet"), Model->GetNetStats().PullRequests, 0);
	ClientHost.PollUntil([&Lost] { return Lost.Delivered > 0; });
	TestFalse(TEXT("data: the attempt failed"), Lost.Last.bTransportOk);
	TestEqual(TEXT("data: the container is pulled"), Model->GetNetStats().PullRequests, 1);
	return true;
}

// Each self-invoking flavour marks when the attempt is sent, so an echo that beats the response is dropped; the
// response does not mark again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoBeforeResponseTest,
	"CrowdySDK.GameModel.SelfEcho.EchoBeforeResponseDropped", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoBeforeResponseTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;

	FSelfEchoTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();

	FSelfEchoOutcome OnContainer;
	Model->InvokeOnContainer(TEXT("c1"), TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(OnContainer));
	TestEqual(TEXT("the container invoke is marked when sent"), Stats.SelfInvokesMarked, 1);
	Model->HandleModelChangeHintForTest(SelfEchoHint(TEXT("c1")));
	TestEqual(TEXT("its echo before the response is dropped"), Stats.SelfEchoesDropped, 1);
	TestEqual(TEXT("and not counted as a hint that pulled"), Stats.HintsAfterSelfInvokeNotDropped, 0);

	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("c2"));
	FSelfEchoOutcome OnEntity;
	Model->InvokeAndApply(NetID, TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(OnEntity));
	TestTrue(TEXT("the entity invoke is marked when sent"), Model->ConsumeSelfEcho(TEXT("c2")));

	Model->TouchCollectionParentForTest(TEXT("c3"), TEXT("SelfEchoType"));
	TestTrue(TEXT("the collection touch is marked when sent"), Model->ConsumeSelfEcho(TEXT("c3")));

	// Every response, the touch's included, has landed before the marks are counted again.
	const TSharedPtr<FCrowdyCppClient> Client = ClientHost.Client;
	ClientHost.PollUntil([&Client] { return Client->NumPendingRequests() == 0; });
	TestTrue(TEXT("the container invoke committed"), OnContainer.Last.bTransportOk && OnContainer.Last.bSuccess);
	TestTrue(TEXT("the entity invoke committed"), OnEntity.Last.bTransportOk && OnEntity.Last.bSuccess);
	TestEqual(TEXT("three attempts, three marks, none added by a response"), Stats.SelfInvokesMarked, 3);
	TestFalse(TEXT("a committed response does not mark again"), Model->ConsumeSelfEcho(TEXT("c1")));
	TestFalse(TEXT("nor for the entity"), Model->ConsumeSelfEcho(TEXT("c2")));
	TestFalse(TEXT("nor for the touch"), Model->ConsumeSelfEcho(TEXT("c3")));
	return true;
}

// An attempt that wrote nothing to its own container gives its mark back, so its echo, if any, pulls; one that
// committed a write to it keeps the mark.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoUncommittedUnmarksTest,
	"CrowdySDK.GameModel.SelfEcho.UncommittedAttemptUnmarks", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoUncommittedUnmarksTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;

	struct FCase
	{
		const TCHAR* Name;
		int32 Status;
		FString Body;
		bool bCommits;
		bool bKeepsMark;
	};
	const FCase Cases[] = {
		{ TEXT("committed a write"), 200, SelfEchoSuccessBody, true, true },
		{ TEXT("committed, wrote nothing"), 200, SelfEchoNoWriteBody, true, false },
		{ TEXT("committed, wrote only another container"), 200, SelfEchoOtherWriteBody, true, false },
		{ TEXT("thrown refusal"), 200, SelfEchoRefusalBody(TEXT("PLATFORM_ERROR")), false, false },
		{ TEXT("in-band fault"), 200, SelfEchoInBandFaultBody, false, false },
		{ TEXT("http failure"), 500, TEXT(""), false, false },
	};
	for (const FCase& Case : Cases)
	{
		FSelfEchoTestWorld Env;
		UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
		FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);
		ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(Case.Status, Case.Body) });

		FSelfEchoOutcome Outcome;
		Model->InvokeOnContainer(TEXT("c1"), TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(Outcome));
		ClientHost.PollUntil([&Outcome] { return Outcome.Delivered > 0; });
		TestEqual(FString::Printf(TEXT("%s: delivered once"), Case.Name), Outcome.Delivered, 1);
		TestEqual(FString::Printf(TEXT("%s: committed as expected"), Case.Name),
			Outcome.Last.bTransportOk && Outcome.Last.bSuccess, Case.bCommits);
		TestEqual(FString::Printf(TEXT("%s: one attempt marked"), Case.Name), Model->GetNetStats().SelfInvokesMarked, 1);
		TestEqual(FString::Printf(TEXT("%s: the echo is dropped only when the attempt committed a write"), Case.Name),
			Model->ConsumeSelfEcho(TEXT("c1")), Case.bKeepsMark);
	}
	return true;
}

// A busy refusal gives its mark back and the retry marks again when it is sent, so the invoke nets one mark.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoBusyRetryTest,
	"CrowdySDK.GameModel.SelfEcho.BusyRetryNetsOneMark", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoBusyRetryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;

	FSelfEchoTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);
	ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, SelfEchoRefusalBody(TEXT("PLATFORM_BUSY"))),
		TPair<int32, FString>(200, SelfEchoRefusalBody(TEXT("PLATFORM_BUSY"))) });
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();

	FSelfEchoOutcome Outcome;
	Model->InvokeOnContainer(TEXT("c1"), TEXT("self_echo_fn"), MakeShared<FJsonObject>(), FString(), CountInto(Outcome));
	for (int32 Round = 0; Round < 12 && Outcome.Delivered == 0; ++Round)
	{
		ClientHost.Client->Poll();
		Env.FireTimers();
	}
	TestTrue(TEXT("the invoke committed after two busy refusals"), Outcome.Last.bTransportOk && Outcome.Last.bSuccess);
	TestEqual(TEXT("two retries"), Stats.InvokeBusyRetries, 2);
	TestEqual(TEXT("each attempt was marked when sent"), Stats.SelfInvokesMarked, 3);
	TestTrue(TEXT("the committed attempt's echo is dropped"), Model->ConsumeSelfEcho(TEXT("c1")));
	TestFalse(TEXT("and no refused attempt left a mark"), Model->ConsumeSelfEcho(TEXT("c1")));
	return true;
}

// Callers merged into one window share one attempt, so they add one mark between them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoMergedCallersTest,
	"CrowdySDK.GameModel.SelfEcho.MergedCallersShareOneMark", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoMergedCallersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;

	FSelfEchoTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);

	constexpr int32 Callers = 3;
	FSelfEchoOutcome Outcomes[Callers];
	for (int32 Index = 0; Index < Callers; ++Index)
	{
		FCrowdyCoalesceRequest Request;
		Request.ContainerId = TEXT("c1");
		Request.FunctionName = TEXT("self_echo_fn");
		Request.Params = MakeShared<FJsonObject>();
		Request.Params->SetNumberField(TEXT("amount"), 2);
		Request.AccumulateParam = TEXT("amount");
		Request.WindowSeconds = 1.0f;
		Model->EnqueueCoalescedInvoke(Request, CountInto(Outcomes[Index]));
	}
	TestEqual(TEXT("the callers share one window"), Model->GetOpenCoalesceWindowCountForTest(), 1);
	TestEqual(TEXT("nothing is marked before the window closes"), Model->GetNetStats().SelfInvokesMarked, 0);

	Env.FireTimers();
	ClientHost.PollUntil([&Outcomes] { return Outcomes[Callers - 1].Delivered > 0; });
	for (int32 Index = 0; Index < Callers; ++Index)
	{
		TestEqual(FString::Printf(TEXT("caller %d hears once"), Index), Outcomes[Index].Delivered, 1);
	}
	TestEqual(TEXT("one attempt, one mark"), Model->GetNetStats().SelfInvokesMarked, 1);
	TestTrue(TEXT("its echo is dropped"), Model->ConsumeSelfEcho(TEXT("c1")));
	TestFalse(TEXT("and only its echo"), Model->ConsumeSelfEcho(TEXT("c1")));
	return true;
}

// The raw Invoke applies nothing locally, so it marks nothing and its echo pulls.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelSelfEchoRawInvokeTest,
	"CrowdySDK.GameModel.SelfEcho.RawInvokeMarksNothing", CrowdySelfEchoTestSupport::SelfEchoTestFlags)
bool FCrowdyGameModelSelfEchoRawInvokeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySelfEchoTestSupport;

	FSelfEchoTestWorld Env;
	UCrowdyGameModelSubsystem* Model = MakeSelfEchoLiveModel(Env.World);
	FCrowdyGameModelTestClientHost ClientHost(Model, SelfEchoTestEndpoint, SelfEchoSuccessBody);

	FCrowdyInvokeRequest Request;
	Request.FunctionName = TEXT("self_echo_fn");
	Request.SelfContainerId = TEXT("c1");
	FSelfEchoOutcome Outcome;
	Model->Invoke(Request, CountInto(Outcome));
	TestEqual(TEXT("nothing marked when sent"), Model->GetNetStats().SelfInvokesMarked, 0);
	TestFalse(TEXT("its echo before the response pulls"), Model->ConsumeSelfEcho(TEXT("c1")));

	ClientHost.PollUntil([&Outcome] { return Outcome.Delivered > 0; });
	TestTrue(TEXT("the invoke committed"), Outcome.Last.bTransportOk && Outcome.Last.bSuccess);
	TestEqual(TEXT("nothing marked on the response"), Model->GetNetStats().SelfInvokesMarked, 0);
	TestFalse(TEXT("its echo after the response pulls"), Model->ConsumeSelfEcho(TEXT("c1")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
