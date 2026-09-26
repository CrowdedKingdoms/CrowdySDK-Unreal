#include "CrowdyCppClient.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

// WITH_METADATA as well as the test flag: the entity cases apply through the test target's CrowdyModel metadata, which
// a game target does not carry.
#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"

// One pull that applies is in flight per container, with at most one more held behind it, and a pull never rolls
// back a key an invoke wrote after the pull was sent.
namespace CrowdyPullInFlightTestSupport
{
	constexpr EAutomationTestFlags PullInFlightTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const TCHAR* const PullInFlightEndpoint = TEXT("https://game.test");

	FString PullInFlightStateBody(int32 Hp, int32 Mana)
	{
		return FString::Printf(
			TEXT("{\"data\":{\"gameModelContainerState\":{\"containerId\":\"c-hot\",\"propertiesJson\":\"{\\\"hp\\\":%d,\\\"mana\\\":%d}\"}}}"),
			Hp, Mana);
	}

	FCrowdyModelChangeHint PullInFlightHint(const FString& ContainerId)
	{
		FCrowdyModelChangeHint Hint;
		Hint.ContainerId = ContainerId;
		return Hint;
	}

	TArray<FCrowdyMutationApplied> PullInFlightMutation(const FString& Key, const FString& NewValueJson)
	{
		TArray<FCrowdyMutationApplied> Mutations;
		FCrowdyMutationApplied& Mutation = Mutations.AddDefaulted_GetRef();
		Mutation.Key = Key;
		Mutation.NewValueJson = NewValueJson;
		return Mutations;
	}

	// A live model with an entity subsystem and a canned client that answers every state read with hp 87, mana 10,
	// counting each request the client sends.
	struct FPullInFlightRig
	{
		UCrowdyEntitySubsystem* Entities = nullptr;
		UCrowdyGameModelSubsystem* Model = nullptr;
		FCrowdyGameModelTestClientHost ClientHost;
		FGuid AnchorNetID;
		int32 Sends = 0;

		explicit FPullInFlightRig(UObject* ModelOuter = GetTransientPackage())
			: Entities(NewObject<UCrowdyEntitySubsystem>(GetTransientPackage()))
			, Model(NewObject<UCrowdyGameModelSubsystem>(ModelOuter))
			, ClientHost(Model, PullInFlightEndpoint, PullInFlightStateBody(87, 10))
		{
			Entities->SetLocalPlayerID(FGuid::NewGuid());
			Model->SetEntitySubsystemForTest(Entities);
			Model->BeginWorldSessionForTest();
			Model->SetApiContextForTest(PullInFlightEndpoint, TEXT("test-token"), 42);
			ClientHost.Client->SetTestOnRequest([this](const FString&) { ++Sends; });
		}

		~FPullInFlightRig()
		{
			ClientHost.Client->SetTestOnRequest(nullptr);
		}

		// Each target is enrolled under one anchor, since a Host participant's id is derived from its class alone.
		FGuid Bind(const FString& ContainerId, UCrowdyGameModelTestTarget*& OutTarget)
		{
			if (!AnchorNetID.IsValid())
			{
				AnchorNetID = Entities->RegisterParticipant(NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage()),
					ECrowdyOwnership::Host);
			}
			OutTarget = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
			const FGuid NetID = Entities->RegisterSubParticipant(OutTarget, AnchorNetID);
			Model->BindEntityContainer(NetID, ContainerId);
			return NetID;
		}

		int32 Pending() const { return ClientHost.Client->NumPendingRequests(); }
		void Poll() { ClientHost.Client->Poll(); }
		const FCrowdyGameModelNetStats& Stats() const { return Model->GetNetStats(); }

		// Polls until Done or two seconds pass, sleeping between polls so a parked retry can fall due.
		template <typename FDonePredicate>
		bool PollFor(FDonePredicate Done)
		{
			for (int32 Attempt = 0; Attempt < 200 && !Done(); ++Attempt)
			{
				Poll();
				FPlatformProcess::Sleep(0.01f);
			}
			return Done();
		}
	};

	FString PullInFlightInvokeBody(const FString& ContainerId, const FString& Key, const FString& NewValueJson)
	{
		return FString::Printf(
			TEXT("{\"data\":{\"gameModelInvoke\":{\"success\":true,\"returnValueJson\":\"\",\"errorMessage\":\"\",\"mutationsApplied\":[{\"containerId\":\"%s\",\"key\":\"%s\",\"oldValueJson\":\"\",\"newValueJson\":\"%s\"}]}}}"),
			*ContainerId, *Key, *NewValueJson);
	}

	const TCHAR* const PullInFlightBusyBody =
		TEXT("{\"errors\":[{\"message\":\"Refused.\",\"extensions\":{\"code\":\"PLATFORM_BUSY\",\"blame\":\"PLATFORM\",\"retryable\":true,\"retryAfterMs\":0}}],\"data\":null}");

	// A coalesce window needs a world with a timer manager to open against.
	struct FPullInFlightTestWorld
	{
		UWorld* World = nullptr;

		FPullInFlightTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FPullInFlightTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightNotificationsHeldTest,
	"CrowdySDK.GameModel.PullInFlight.NotificationsHeldBehindOnePull", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightNotificationsHeldTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	Rig.Bind(TEXT("c-hot"), Target);

	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-hot")));
	Rig.Model->DrainRefreshPullsForTest();
	if (!TestEqual(TEXT("the first notification pulls"), Rig.Sends, 1))
	{
		return false;
	}

	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-hot")));
	Rig.Model->DrainRefreshPullsForTest();
	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-hot")));
	Rig.Model->DrainRefreshPullsForTest();
	TestEqual(TEXT("two notifications while it is out send nothing"), Rig.Sends, 1);
	TestEqual(TEXT("only one request is in flight"), Rig.Pending(), 1);
	TestEqual(TEXT("both are held behind it"), Rig.Stats().PullsHeldBehindInFlight, 2);

	Rig.Poll();
	TestEqual(TEXT("the landed pull applied"), Target->Hp, 87);
	TestEqual(TEXT("one follow-up is owed"), Rig.Stats().FollowUpPulls, 1);
	TestEqual(TEXT("and it waits for the merge window"), Rig.Model->GetPendingRefreshPullCountForTest(), 1);
	TestEqual(TEXT("so nothing is sent yet"), Rig.Sends, 1);

	Rig.Model->DrainRefreshPullsForTest();
	TestEqual(TEXT("the window sends the one follow-up"), Rig.Sends, 2);
	Rig.Poll();
	TestEqual(TEXT("and no second one follows it"), Rig.Stats().FollowUpPulls, 1);
	TestEqual(TEXT("nothing is left waiting"), Rig.Model->GetPendingRefreshPullCountForTest(), 0);
	TestEqual(TEXT("nor in flight"), Rig.Pending(), 0);
	TestEqual(TEXT("no pull overlapped another"), Rig.Stats().OverlappingPulls, 0);

	TArray<FString> Lines;
	Rig.Model->DescribeNetStats(Lines);
	TestTrue(TEXT("the description prints the held and follow-up counts"),
		Lines.Contains(TEXT("[GameModel] pulls held behind in flight 2 follow-ups 1 stale keys skipped 0")));
	Rig.Model->ResetNetStats();
	TestTrue(TEXT("a reset clears them"), Rig.Stats().PullsHeldBehindInFlight == 0 && Rig.Stats().FollowUpPulls == 0
		&& Rig.Stats().StaleKeysSkipped == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightExplicitFollowUpTest,
	"CrowdySDK.GameModel.PullInFlight.ExplicitFollowUpSkipsWindow", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightExplicitFollowUpTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	const FGuid NetID = Rig.Bind(TEXT("c-hot"), Target);

	Rig.Model->HandleModelChanged(NetID);
	Rig.Model->HandleModelChanged(NetID);
	TestEqual(TEXT("the second explicit pull is held"), Rig.Sends, 1);
	TestEqual(TEXT("and counted as held"), Rig.Stats().PullsHeldBehindInFlight, 1);

	Rig.Poll();
	TestEqual(TEXT("its follow-up is sent as soon as the first lands"), Rig.Sends, 2);
	TestEqual(TEXT("without the merge window"), Rig.Model->GetPendingRefreshPullCountForTest(), 0);
	TestEqual(TEXT("one follow-up counted"), Rig.Stats().FollowUpPulls, 1);
	TestEqual(TEXT("one request in flight"), Rig.Pending(), 1);

	Rig.Poll();
	TestEqual(TEXT("nothing further is sent"), Rig.Sends, 2);
	TestEqual(TEXT("nothing is in flight"), Rig.Pending(), 0);
	TestEqual(TEXT("no pull overlapped another"), Rig.Stats().OverlappingPulls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightBulkLeavesOutTest,
	"CrowdySDK.GameModel.PullInFlight.BulkLeavesOutInFlight", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightBulkLeavesOutTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* TargetA = nullptr;
	UCrowdyGameModelTestTarget* TargetB = nullptr;
	UCrowdyGameModelTestTarget* TargetC = nullptr;
	const FGuid NetA = Rig.Bind(TEXT("c-a"), TargetA);
	Rig.Bind(TEXT("c-b"), TargetB);
	Rig.Bind(TEXT("c-c"), TargetC);

	Rig.Model->HandleModelChanged(NetA);
	Rig.ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, TEXT(
		"{\"data\":{\"gameModelContainerStates\":["
		"{\"containerId\":\"c-b\",\"propertiesJson\":\"{\\\"hp\\\":61}\"},"
		"{\"containerId\":\"c-c\",\"propertiesJson\":\"{\\\"hp\\\":62}\"}]}}")) });
	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-a")));
	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-b")));
	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-c")));
	Rig.Model->DrainRefreshPullsForTest();

	FString Body;
	Rig.ClientHost.Client->GetLastTestRequestBody(Body);
	TestEqual(TEXT("the drain sent one bulk read"), Rig.Sends, 2);
	TestTrue(TEXT("for the containers not in flight"), Body.Contains(TEXT("c-b")) && Body.Contains(TEXT("c-c")));
	TestFalse(TEXT("leaving out the one already out"), Body.Contains(TEXT("c-a")));
	TestEqual(TEXT("which is held behind its pull"), Rig.Stats().PullsHeldBehindInFlight, 1);

	Rig.Poll();
	TestTrue(TEXT("the bulk rows applied"), TargetB->Hp == 61 && TargetC->Hp == 62);
	TestEqual(TEXT("the single pull applied"), TargetA->Hp, 87);
	TestEqual(TEXT("the held container follows up"), Rig.Stats().FollowUpPulls, 1);
	TestEqual(TEXT("through the merge window"), Rig.Model->GetPendingRefreshPullCountForTest(), 1);
	TestEqual(TEXT("no pull overlapped another"), Rig.Stats().OverlappingPulls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightFailureFollowsUpTest,
	"CrowdySDK.GameModel.PullInFlight.FailedPullStillFollowsUp", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightFailureFollowsUpTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	const FGuid NetID = Rig.Bind(TEXT("c-hot"), Target);

	Rig.ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, TEXT(
		"{\"errors\":[{\"message\":\"Refused.\",\"extensions\":{\"code\":\"FORBIDDEN\",\"blame\":\"AUTHOR\",\"retryable\":false}}],\"data\":null}")) });
	Rig.Model->HandleModelChanged(NetID);
	Rig.Model->HandleModelChanged(NetID);
	TestEqual(TEXT("one request out"), Rig.Sends, 1);

	Rig.Poll();
	TestEqual(TEXT("the failed pull applied nothing"), Target->Hp, 100);
	TestEqual(TEXT("its record cleared and the held pull followed up"), Rig.Sends, 2);
	TestEqual(TEXT("one follow-up counted"), Rig.Stats().FollowUpPulls, 1);

	Rig.Poll();
	TestEqual(TEXT("the follow-up applied"), Target->Hp, 87);

	Rig.Model->HandleModelChanged(NetID);
	TestEqual(TEXT("a later pull is not held by a stale record"), Rig.Sends, 3);
	Rig.Poll();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightDataCallerOnceTest,
	"CrowdySDK.GameModel.PullInFlight.HeldCallerAnsweredByFollowUp", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightDataCallerOnceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;

	int32 FirstAnswers = 0;
	int32 HeldAnswers = 0;
	bool bHeldOk = false;
	Rig.Model->PullDataContainer(TEXT("d-1"), [&FirstAnswers](bool) { ++FirstAnswers; });
	Rig.Model->PullDataContainer(TEXT("d-1"), [&HeldAnswers, &bHeldOk](bool bOk)
	{
		++HeldAnswers;
		bHeldOk = bOk;
	});
	TestEqual(TEXT("the second caller is held"), Rig.Sends, 1);

	Rig.Poll();
	TestEqual(TEXT("the first caller is answered by its own pull"), FirstAnswers, 1);
	TestEqual(TEXT("the held caller is not answered by a read sent before its call"), HeldAnswers, 0);
	TestEqual(TEXT("its follow-up is sent"), Rig.Sends, 2);

	Rig.Poll();
	TestEqual(TEXT("the held caller is answered by the follow-up"), HeldAnswers, 1);
	TestTrue(TEXT("with its outcome"), bHeldOk);
	TestEqual(TEXT("the first caller is not answered again"), FirstAnswers, 1);

	Rig.Poll();
	TestTrue(TEXT("each caller is answered exactly once"), FirstAnswers == 1 && HeldAnswers == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightStaleKeysTest,
	"CrowdySDK.GameModel.PullInFlight.OlderPullKeepsNewerInvokeWrite", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightStaleKeysTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	const FGuid NetID = Rig.Bind(TEXT("c-hot"), Target);

	Rig.Model->HandleModelChanged(NetID);
	TArray<FCrowdyMutationApplied> Mutations = PullInFlightMutation(TEXT("hp"), TEXT("50"));
	Mutations.Append(PullInFlightMutation(TEXT("mana"), TEXT("10")));
	Rig.Model->ApplyInvokeMutations(NetID, TEXT("c-hot"), Mutations, Rig.Model->StampDispatchForTest());
	TestEqual(TEXT("the invoke wrote hp"), Target->Hp, 50);

	Rig.Poll();
	TestEqual(TEXT("the pull sent before the invoke leaves hp alone"), Target->Hp, 50);
	TestEqual(TEXT("and applies every other key"), Target->Mana, 10);
	TestEqual(TEXT("hp fired its OnRep once, for the invoke"), Target->HpOnRepCount, 1);
	TestEqual(TEXT("only the kept key the read disagreed with counts as stale"), Rig.Stats().StaleKeysSkipped, 1);
	TestEqual(TEXT("it queues one follow-up"), Rig.Stats().FollowUpPulls, 1);
	TestEqual(TEXT("through the merge window"), Rig.Model->GetPendingRefreshPullCountForTest(), 1);
	FString CachedHp;
	TestTrue(TEXT("the cache keeps the newer value"),
		Rig.Model->TryGetCachedValueJson(NetID, TEXT("hp"), CachedHp) && CachedHp == TEXT("50"));

	Rig.Model->DrainRefreshPullsForTest();
	Rig.Poll();
	TestEqual(TEXT("the follow-up applies hp normally"), Target->Hp, 87);
	TestEqual(TEXT("and skips nothing"), Rig.Stats().StaleKeysSkipped, 1);
	TestEqual(TEXT("nor queues another"), Rig.Model->GetPendingRefreshPullCountForTest(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightSlowInvokeTest,
	"CrowdySDK.GameModel.PullInFlight.InvokeSentBeforePullDoesNotShadowIt", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightSlowInvokeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	const FGuid NetID = Rig.Bind(TEXT("c-hot"), Target);

	// The invoke is sent first and answers while the pull is out; the pull read after it, so its value is newer.
	Rig.ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, PullInFlightInvokeBody(TEXT("c-hot"), TEXT("hp"), TEXT("50"))) });
	Rig.Model->InvokeAndApply(NetID, TEXT("fn"), nullptr, FString(), nullptr);
	Rig.Model->HandleModelChanged(NetID);
	TestEqual(TEXT("invoke and pull are both out"), Rig.Sends, 2);

	Rig.Poll();
	TestEqual(TEXT("the pull sent after the invoke applies its value"), Target->Hp, 87);
	TestEqual(TEXT("nothing was kept"), Rig.Stats().StaleKeysSkipped, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightInvokeAfterPullTest,
	"CrowdySDK.GameModel.PullInFlight.InvokeSentAfterPullIsKept", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightInvokeAfterPullTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	const FGuid NetID = Rig.Bind(TEXT("c-hot"), Target);

	// The pull is refused busy and parked, so the invoke sent after it answers first, through the real send path.
	Rig.ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, PullInFlightBusyBody),
		TPair<int32, FString>(200, PullInFlightInvokeBody(TEXT("c-hot"), TEXT("hp"), TEXT("50"))) });
	Rig.Model->HandleModelChanged(NetID);
	Rig.Model->InvokeAndApply(NetID, TEXT("fn"), nullptr, FString(), nullptr);
	Rig.Poll();
	TestEqual(TEXT("the invoke applied"), Target->Hp, 50);

	const bool bLanded = Rig.PollFor([&Rig] { return Rig.Pending() == 0; });
	TestTrue(TEXT("the parked pull was sent again and landed"), bLanded);
	TestEqual(TEXT("its read leaves the invoke's write in place"), Target->Hp, 50);
	TestEqual(TEXT("and queues one follow-up"), Rig.Model->GetPendingRefreshPullCountForTest(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightRejectedWriteTest,
	"CrowdySDK.GameModel.PullInFlight.RejectedWriteIsNotKept", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightRejectedWriteTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	const FGuid NetID = Rig.Bind(TEXT("c-hot"), Target);

	Rig.Model->HandleModelChanged(NetID);
	Rig.Model->ApplyInvokeMutations(NetID, TEXT("c-hot"), PullInFlightMutation(TEXT("hp"), TEXT("{broken")),
		Rig.Model->StampDispatchForTest());
	TestEqual(TEXT("the unreadable value was not written"), Target->Hp, 100);

	Rig.Poll();
	TestEqual(TEXT("so the pull applies hp"), Target->Hp, 87);
	TestEqual(TEXT("and nothing was kept"), Rig.Stats().StaleKeysSkipped, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightSharedRowTest,
	"CrowdySDK.GameModel.PullInFlight.TwoEntitiesOneRow", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightSharedRowTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	AddExpectedMessagePlain(TEXT("is now bound by entity"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* TargetA = nullptr;
	UCrowdyGameModelTestTarget* TargetB = nullptr;
	const FGuid NetA = Rig.Bind(TEXT("c-hot"), TargetA);
	const FGuid NetB = Rig.Bind(TEXT("c-hot"), TargetB);

	Rig.ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, PullInFlightStateBody(11, 1)),
		TPair<int32, FString>(200, PullInFlightStateBody(22, 2)) });
	Rig.Model->HandleModelChanged(NetA);
	Rig.Model->HandleModelChanged(NetB);
	Rig.Model->HandleModelChanged(NetA);
	TestEqual(TEXT("one read of the shared row is out"), Rig.Sends, 1);
	Rig.Model->ApplyMutationsToContainer(NetB, TargetB, PullInFlightMutation(TEXT("hp"), TEXT("50")),
		Rig.Model->StampDispatchForTest());

	Rig.Poll();
	TestEqual(TEXT("B's write does not stop A taking the read"), TargetA->Hp, 11);
	TestEqual(TEXT("both held entities follow up in one read"), Rig.Sends, 2);

	Rig.Poll();
	TestEqual(TEXT("the follow-up reaches A"), TargetA->Hp, 22);
	TestEqual(TEXT("and B, whose held pull is not dropped"), TargetB->Hp, 22);
	TestEqual(TEXT("no read of the row overlapped another"), Rig.Stats().OverlappingPulls, 0);
	return true;
}

// A row bound to an entity and also watched by id: one notification, one read, both caches and both delegates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightBoundAndWatchedRowTest,
	"CrowdySDK.GameModel.PullInFlight.BoundAndWatchedRowRefreshesBoth", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightBoundAndWatchedRowTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	Rig.Bind(TEXT("c-hot"), Target);
	Rig.Model->WatchDataContainer(TEXT("c-hot"));
	Rig.Model->OnDataContainerChanged.AddDynamic(Target, &UCrowdyGameModelTestTarget::HandleDataContainerChanged);

	FString Hp;
	TestFalse(TEXT("the by-id cache starts empty"), Rig.Model->TryGetContainerValueJson(TEXT("c-hot"), TEXT("hp"), Hp));

	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-hot")));
	Rig.Model->DrainRefreshPullsForTest();
	Rig.Poll();

	TestEqual(TEXT("one read serves both"), Rig.Sends, 1);
	TestEqual(TEXT("the entity takes the read"), Target->Hp, 87);
	TestTrue(TEXT("the by-id cache takes the same read"),
		Rig.Model->TryGetContainerValueJson(TEXT("c-hot"), TEXT("hp"), Hp) && Hp == TEXT("87"));
	TestEqual(TEXT("the by-id delegate fires once"), Target->DataChangedCount, 1);
	TestEqual(TEXT("naming the row"), Target->LastChangedContainerId, FString(TEXT("c-hot")));
	return true;
}

// A by-id write that lands while the bound row's read is out survives that older read in the by-id cache.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightBoundAndWatchedKeepsByIdWriteTest,
	"CrowdySDK.GameModel.PullInFlight.BoundAndWatchedRowKeepsNewerByIdWrite", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightBoundAndWatchedKeepsByIdWriteTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	Rig.Bind(TEXT("c-hot"), Target);
	Rig.Model->WatchDataContainer(TEXT("c-hot"));

	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-hot")));
	Rig.Model->DrainRefreshPullsForTest();
	Rig.Model->ApplyDataContainerMutations(TEXT("c-hot"), PullInFlightMutation(TEXT("hp"), TEXT("50")),
		Rig.Model->StampDispatchForTest());
	Rig.Poll();

	FString Hp;
	TestTrue(TEXT("the older read does not roll the by-id write back"),
		Rig.Model->TryGetContainerValueJson(TEXT("c-hot"), TEXT("hp"), Hp) && Hp == TEXT("50"));
	TestEqual(TEXT("and a follow-up read is owed"), Rig.Model->GetPendingRefreshPullCountForTest(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightUnwatchedWriteTest,
	"CrowdySDK.GameModel.PullInFlight.WriteBeforeFirstDataPullLandsFollowsUp", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightUnwatchedWriteTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;

	Rig.Model->PullDataContainer(TEXT("d-1"), nullptr);
	TArray<FCrowdyMutationApplied> Mutations = PullInFlightMutation(TEXT("hp"), TEXT("50"));
	Mutations[0].ContainerId = TEXT("d-1");
	Rig.Model->ApplyInvokeMutations(FGuid(), TEXT("c-other"), Mutations, Rig.Model->StampDispatchForTest());

	Rig.Poll();
	TestEqual(TEXT("the first pull is followed by one more"), Rig.Sends, 2);
	TestEqual(TEXT("counted as a follow-up"), Rig.Stats().FollowUpPulls, 1);
	Rig.Poll();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightDataStaleKeysTest,
	"CrowdySDK.GameModel.PullInFlight.OlderDataPullKeepsNewerInvokeWrite", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightDataStaleKeysTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;

	Rig.Model->PullDataContainer(TEXT("d-1"), nullptr);
	TArray<FCrowdyMutationApplied> Mutations = PullInFlightMutation(TEXT("hp"), TEXT("50"));
	Mutations.Append(PullInFlightMutation(TEXT("slot"), TEXT("\"sword\"")));
	Rig.Model->ApplyDataContainerMutations(TEXT("d-1"), Mutations, Rig.Model->StampDispatchForTest());

	Rig.Poll();
	TestEqual(TEXT("a kept key the read disagreed with sends one follow-up"), Rig.Sends, 2);
	FString Hp;
	FString Mana;
	FString Slot;
	TestTrue(TEXT("hp keeps the invoke's value"), Rig.Model->TryGetContainerValueJson(TEXT("d-1"), TEXT("hp"), Hp) && Hp == TEXT("50"));
	TestTrue(TEXT("mana takes the pulled value"), Rig.Model->TryGetContainerValueJson(TEXT("d-1"), TEXT("mana"), Mana) && Mana == TEXT("10"));
	TestTrue(TEXT("a key the older pull does not carry is not removed"),
		Rig.Model->TryGetContainerValueJson(TEXT("d-1"), TEXT("slot"), Slot) && Slot == TEXT("\"sword\""));
	TestEqual(TEXT("both written keys were left alone"), Rig.Stats().StaleKeysSkipped, 2);
	Rig.Poll();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightPlainReadTest,
	"CrowdySDK.GameModel.PullInFlight.PlainReadIsNeverHeld", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightPlainReadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	const FGuid NetID = Rig.Bind(TEXT("c-hot"), Target);

	Rig.Model->HandleModelChanged(NetID);
	int32 Reads = 0;
	Rig.Model->PullContainerState(TEXT("c-hot"), [&Reads](bool bOk, TSharedPtr<FJsonObject>) { Reads += bOk ? 1 : 100; });
	TestEqual(TEXT("a read that applies nothing is sent at once"), Rig.Sends, 2);
	TestEqual(TEXT("and is never held"), Rig.Stats().PullsHeldBehindInFlight, 0);
	TestEqual(TEXT("nor counted as overlapping a pull that applies"), Rig.Stats().OverlappingPulls, 0);

	Rig.Poll();
	TestEqual(TEXT("its caller is answered once, with the read"), Reads, 1);
	TestEqual(TEXT("and it causes no follow-up"), Rig.Stats().FollowUpPulls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightTeardownTest,
	"CrowdySDK.GameModel.PullInFlight.TeardownSendsNoFollowUp", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightTeardownTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	const FGuid NetID = Rig.Bind(TEXT("c-hot"), Target);

	int32 FirstAnswers = 0;
	int32 HeldAnswers = 0;
	bool bHeldOk = true;
	Rig.Model->HandleModelChanged(NetID);
	Rig.Model->HandleModelChanged(NetID);
	Rig.Model->PullDataContainer(TEXT("d-1"), [&FirstAnswers](bool) { ++FirstAnswers; });
	Rig.Model->PullDataContainer(TEXT("d-1"), [&HeldAnswers, &bHeldOk](bool bOk)
	{
		++HeldAnswers;
		bHeldOk = bOk;
	});
	TestEqual(TEXT("two pulls out, two held"), Rig.Sends, 2);

	Rig.Model->FailPendingWorkForTest();
	TestEqual(TEXT("teardown answers the held caller"), HeldAnswers, 1);
	TestFalse(TEXT("with a failure"), bHeldOk);

	Rig.Poll();
	Rig.Poll();
	TestEqual(TEXT("the pull already out still answers its caller"), FirstAnswers, 1);
	TestEqual(TEXT("the held caller is not answered again"), HeldAnswers, 1);
	TestEqual(TEXT("no follow-up is sent after teardown"), Rig.Sends, 2);
	TestEqual(TEXT("nor counted"), Rig.Stats().FollowUpPulls, 0);

	// Once teardown has begun nothing new is pulled, and a data caller is answered at once with a failure.
	Rig.Model->HandleModelChanged(NetID);
	int32 LateAnswers = 0;
	bool bLateOk = true;
	Rig.Model->PullDataContainer(TEXT("d-2"), [&LateAnswers, &bLateOk](bool bOk)
	{
		++LateAnswers;
		bLateOk = bOk;
	});
	TestEqual(TEXT("nothing is sent during teardown"), Rig.Sends, 2);
	TestTrue(TEXT("the late data caller is answered once, with a failure"), LateAnswers == 1 && !bLateOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightTeardownReentryTest,
	"CrowdySDK.GameModel.PullInFlight.TeardownReentryKeepsHeldCallers", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightTeardownReentryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightTestWorld Env;
	FPullInFlightRig Rig(Env.World);

	int32 FirstAnswers = 0;
	int32 HeldAnswers = 0;
	int32 LateAnswers = 0;
	Rig.Model->PullDataContainer(TEXT("d-1"), [&FirstAnswers](bool) { ++FirstAnswers; });
	Rig.Model->PullDataContainer(TEXT("d-1"), [&HeldAnswers](bool) { ++HeldAnswers; });

	// A merge window whose caller, failed by teardown before the pulls are, pulls a held row and a new one.
	FCrowdyCoalesceRequest Request;
	Request.ContainerId = TEXT("c-window");
	Request.FunctionName = TEXT("fn");
	Request.Params = MakeShared<FJsonObject>();
	Request.Params->SetNumberField(TEXT("amount"), 1);
	Request.AccumulateParam = TEXT("amount");
	Request.WindowSeconds = 5.0f;
	UCrowdyGameModelSubsystem* Model = Rig.Model;
	Rig.Model->EnqueueCoalescedInvoke(Request, [Model, &LateAnswers](FCrowdyInvokeResult)
	{
		Model->PullDataContainer(TEXT("d-1"), [&LateAnswers](bool bOk) { LateAnswers += bOk ? 100 : 1; });
		Model->PullDataContainer(TEXT("d-2"), [&LateAnswers](bool bOk) { LateAnswers += bOk ? 100 : 1; });
	});
	TestEqual(TEXT("one pull out"), Rig.Sends, 1);

	Rig.Model->Deinitialize();
	TestEqual(TEXT("both late callers are answered with a failure"), LateAnswers, 2);
	TestEqual(TEXT("the held caller is still answered"), HeldAnswers, 1);
	TestEqual(TEXT("teardown sends nothing"), Rig.Sends, 1);

	Rig.Poll();
	TestEqual(TEXT("the pull already out answers its own caller"), FirstAnswers, 1);
	TestTrue(TEXT("and nobody is answered twice"), HeldAnswers == 1 && LateAnswers == 2);
	return true;
}

namespace CrowdyPullInFlightTestSupport
{
	FString PullInFlightEnsureBody(const FString& ContainerId)
	{
		return FString::Printf(
			TEXT("{\"data\":{\"gameModelEnsureContainer\":{\"container\":{\"containerId\":\"%s\",\"ownerUserId\":null},\"created\":true}}}"),
			*ContainerId);
	}
}

// Every bind completion queues its first pull: a first bind, the sweep's retry and a bulk miss's ensure each send
// no read of their own, and the merge window reads every row they bound in one call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightBindPullsQueuedTest,
	"CrowdySDK.GameModel.PullInFlight.BindPullsGoThroughTheQueue", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightBindPullsQueuedTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	const FGuid OwnAnchor = Rig.Entities->RegisterParticipant(NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage()),
		ECrowdyOwnership::LocalClient);
	const FGuid HostAnchor = Rig.Entities->RegisterParticipant(
		NewObject<UCrowdyGameModelTestTargetDerived>(GetTransientPackage()), ECrowdyOwnership::Host);
	UCrowdyGameModelTestComponent* FirstBind = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	UCrowdyGameModelTestComponent* Retried = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	UCrowdyGameModelTestComponent* Shared = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	const FGuid FirstNetID = Rig.Entities->RegisterSubParticipant(FirstBind, OwnAnchor);
	const FGuid SharedNetID = Rig.Entities->RegisterSubParticipant(Shared, HostAnchor);
	Rig.ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, PullInFlightEnsureBody(TEXT("c-first"))),
		TPair<int32, FString>(200, PullInFlightEnsureBody(TEXT("c-retried"))),
		TPair<int32, FString>(200, PullInFlightEnsureBody(TEXT("c-missed"))) });

	Rig.Model->HandleEntityRegisteredForTest(FirstNetID);
	const FGuid RetriedNetID = Rig.Entities->RegisterSubParticipant(Retried, OwnAnchor);
	Rig.Model->AddPendingModelEntityForTest(RetriedNetID, TEXT("TestAttributes"));
	Rig.Model->RetryPendingModelEntitiesForTest();
	Rig.Model->AddPendingModelEntityForTest(SharedNetID, TEXT("Node"));
	Rig.Model->FinishBulkResolveForTest(TEXT("Node"), FString(), {SharedNetID}, {});
	TestEqual(TEXT("three resolves are out"), Rig.Sends, 3);

	Rig.Poll();
	FString Bound;
	TestTrue(TEXT("all three bound"), Rig.Model->TryGetContainerId(FirstNetID, Bound)
		&& Rig.Model->TryGetContainerId(RetriedNetID, Bound) && Rig.Model->TryGetContainerId(SharedNetID, Bound));
	TestEqual(TEXT("no bind sent a read of its own"), Rig.Sends, 3);
	TestEqual(TEXT("each queued its pull"), Rig.Model->GetPendingRefreshPullCountForTest(), 3);

	Rig.Model->DrainRefreshPullsForTest();
	TestEqual(TEXT("the window reads them in one call"), Rig.Sends, 4);
	FString Body;
	Rig.ClientHost.Client->GetLastTestRequestBody(Body);
	TestTrue(TEXT("carrying every bound row"), Body.Contains(TEXT("c-first")) && Body.Contains(TEXT("c-retried"))
		&& Body.Contains(TEXT("c-missed")));
	Rig.Poll();
	return true;
}

// A queued pull reaches every entity bound to its row, not only the one whose notification queued it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightQueuedPullEveryHolderTest,
	"CrowdySDK.GameModel.PullInFlight.QueuedPullReachesEveryEntityOnTheRow", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightQueuedPullEveryHolderTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	AddExpectedMessagePlain(TEXT("is now bound by entity"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* TargetA = nullptr;
	UCrowdyGameModelTestTarget* TargetB = nullptr;
	Rig.Bind(TEXT("c-hot"), TargetA);
	Rig.Bind(TEXT("c-hot"), TargetB);

	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-hot")));
	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-hot")));
	TestEqual(TEXT("the row is queued once"), Rig.Model->GetPendingRefreshPullCountForTest(), 1);
	Rig.Model->DrainRefreshPullsForTest();
	TestEqual(TEXT("and read once"), Rig.Sends, 1);

	Rig.Poll();
	TestEqual(TEXT("the first entity on the row applies it"), TargetA->Hp, 87);
	TestEqual(TEXT("and so does the second"), TargetB->Hp, 87);
	return true;
}

// A drain disarms its window, so a notification queued while it runs opens a window of its own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightDrainDisarmsTest,
	"CrowdySDK.GameModel.PullInFlight.DrainDisarmsItsWindow", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightDrainDisarmsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightTestWorld Env;
	FPullInFlightRig Rig(Env.World);
	UCrowdyGameModelTestTarget* Target = nullptr;
	Rig.Bind(TEXT("c-hot"), Target);

	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-hot")));
	TestTrue(TEXT("a notification arms the window"), Rig.Model->GetRefreshPullTimerRateForTest() > 0.0f);
	Rig.Model->DrainRefreshPullsForTest();
	TestTrue(TEXT("the drain disarms it"), Rig.Model->GetRefreshPullTimerRateForTest() < 0.0f);
	Rig.Poll();
	return true;
}

// An invoke sent while a pull waits in the merge window is newer than that pull, so the pull keeps its write.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPullInFlightQueuedPullKeepsInvokeTest,
	"CrowdySDK.GameModel.PullInFlight.QueuedPullKeepsAnInvokeSentWhileQueued", CrowdyPullInFlightTestSupport::PullInFlightTestFlags)
bool FCrowdyPullInFlightQueuedPullKeepsInvokeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyPullInFlightTestSupport;
	FPullInFlightRig Rig;
	UCrowdyGameModelTestTarget* Target = nullptr;
	const FGuid NetID = Rig.Bind(TEXT("c-hot"), Target);

	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-hot")));
	const uint64 Invoked = Rig.Model->StampDispatchForTest();
	Rig.Model->HandleModelChangeHintForTest(PullInFlightHint(TEXT("c-hot")));
	Rig.Model->DrainRefreshPullsForTest();
	TestEqual(TEXT("the pull goes out after the invoke"), Rig.Sends, 1);
	Rig.Model->ApplyInvokeMutations(NetID, TEXT("c-hot"), PullInFlightMutation(TEXT("hp"), TEXT("50")), Invoked);
	TestEqual(TEXT("the invoke's answer lands while the pull is out"), Target->Hp, 50);

	Rig.Poll();
	TestEqual(TEXT("the pull leaves the invoke's write in place"), Target->Hp, 50);
	TestEqual(TEXT("and counts the kept key"), Rig.Stats().StaleKeysSkipped, 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
