#include "CrowdyCppClient.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

// WITH_METADATA as well as the test flag: the apply cases write through the test target's CrowdyModel metadata, which
// a game target does not carry, so their change counts would read differently there.
#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"

namespace CrowdyNetStatsTestSupport
{
	constexpr EAutomationTestFlags NetStatsTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	TSharedPtr<FJsonObject> HpManaState(int32 Hp, int32 Mana)
	{
		const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
		State->SetNumberField(TEXT("hp"), Hp);
		State->SetNumberField(TEXT("mana"), Mana);
		return State;
	}

	FCrowdyModelChangeHint ContainerHint(const FString& ContainerId)
	{
		FCrowdyModelChangeHint Hint;
		Hint.ContainerId = ContainerId;
		return Hint;
	}

	// A NewObject subsystem has no world and so no API context; the resolve that fails on it logs one of these.
	void AllowNetStatsMissingApiContext(FAutomationTestBase& Test)
	{
		Test.AddExpectedError(TEXT("Game API endpoint is empty|No UCrowdyGameSession|No app-scoped game token"),
			EAutomationExpectedErrorFlags::Contains, 0);
	}

	// A resolve that has an API context but no game instance stops at the missing client host.
	void AllowNetStatsMissingClientHost(FAutomationTestBase& Test)
	{
		Test.AddExpectedMessagePlain(TEXT("No Game API client host"), ELogVerbosity::Warning,
			EAutomationExpectedMessageFlags::Contains, 0);
	}

	const TCHAR* const NetStatsTestEndpoint = TEXT("https://game.test");

	void SetNetStatsTestApiContext(UCrowdyGameModelSubsystem* Model)
	{
		Model->SetApiContextForTest(NetStatsTestEndpoint, TEXT("test-token"), 42);
	}

	// A real world gives the subsystem a timer manager for the sweep to re-arm on.
	struct FNetStatsTestWorld
	{
		UWorld* World = nullptr;

		FNetStatsTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FNetStatsTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	FCrowdyCppRequestHandle IssueSessionRead(const TSharedPtr<FCrowdyCppClient>& Client)
	{
		return Client->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSession"), MakeShared<FJsonObject>(),
			[](FCrowdyCppJsonResult) {});
	}

	const FCrowdyCppOpStats* FindOp(const TArray<FCrowdyCppOpStats>& Ops, const FString& Operation)
	{
		return Ops.FindByPredicate([&Operation](const FCrowdyCppOpStats& Op) { return Op.Operation == Operation; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsRedundantPullTest,
	"CrowdySDK.GameModel.NetStats.RedundantPullApplies", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsRedundantPullTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	UCrowdyGameModelTestTarget* Target = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model) || !TestNotNull(TEXT("target created"), Target))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();
	Model->ApplyStateToContainer(NetID, Target, HpManaState(87, 50));
	TestEqual(TEXT("the first apply changed hp, so it is not redundant"), Target->HpOnRepCount, 1);
	Model->ApplyStateToContainer(NetID, Target, HpManaState(87, 50));

	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();
	TestEqual(TEXT("both entity applies counted"), Stats.PullApplies, 2);
	TestEqual(TEXT("only the unchanged re-apply is redundant"), Stats.RedundantPullApplies, 1);

	const TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
	Slot->SetStringField(TEXT("slot0"), TEXT("sword"));
	Model->ApplyDataContainerState(TEXT("data-1"), Slot);
	Model->ApplyDataContainerState(TEXT("data-1"), Slot);
	TestEqual(TEXT("data container applies are counted too"), Stats.PullApplies, 4);
	TestEqual(TEXT("the unchanged data re-apply is redundant"), Stats.RedundantPullApplies, 2);

	// A mutation echo is not a pull, so it leaves both counters alone.
	FCrowdyMutationApplied Mutation;
	Mutation.Key = TEXT("hp");
	Mutation.NewValueJson = TEXT("80");
	Model->ApplyMutationsToContainer(NetID, Target, {Mutation});
	TestEqual(TEXT("a confirmed-invoke apply is not a pull apply"), Stats.PullApplies, 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsSelfEchoTest,
	"CrowdySDK.GameModel.NetStats.SelfEcho", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsSelfEchoTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();

	Model->MarkSelfActed(TEXT("c1"));
	TestEqual(TEXT("one self-invoke marked"), Stats.SelfInvokesMarked, 1);

	Model->HandleModelChangeHintForTest(ContainerHint(TEXT("c1")));
	TestEqual(TEXT("the first echo is dropped"), Stats.SelfEchoesDropped, 1);
	TestEqual(TEXT("a dropped echo is not counted as one that pulled"), Stats.HintsAfterSelfInvokeNotDropped, 0);

	Model->HandleModelChangeHintForTest(ContainerHint(TEXT("c1")));
	Model->HandleModelChangeHintForTest(ContainerHint(TEXT("c1")));
	TestEqual(TEXT("only one echo is dropped per mark"), Stats.SelfEchoesDropped, 1);
	TestEqual(TEXT("the later hints inside the window are counted"), Stats.HintsAfterSelfInvokeNotDropped, 2);

	Model->HandleModelChangeHintForTest(ContainerHint(TEXT("c2")));
	TestEqual(TEXT("a hint for a container never invoked is not counted"), Stats.HintsAfterSelfInvokeNotDropped, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsReResolveTest,
	"CrowdySDK.GameModel.NetStats.ReResolve", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsReResolveTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;
	AllowNetStatsMissingApiContext(*this);
	AllowNetStatsMissingClientHost(*this);

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}
	// Live, so a resolve that fails for want of a client releases its in-flight guard and the same entity can go again.
	Model->BeginWorldSessionForTest();
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();

	const FGuid BoundBefore = FGuid::NewGuid();
	Model->BindEntityContainer(BoundBefore, TEXT("c1"));
	Model->UnbindEntityContainerForTest(BoundBefore);
	Model->ResolveOrCreateContainer(BoundBefore, TEXT("NetStatsType"), FString(), nullptr);
	TestEqual(TEXT("a resolve with no API context never goes out, so it is not counted"), Stats.ResolveStarts, 0);
	TestEqual(TEXT("nor counted as a re-resolve"), Stats.ReResolves, 0);

	// A shared entity ensures its row; an entity this client neither owns nor shares reads it by key.
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	Model->SetEntitySubsystemForTest(Entities);
	const FGuid Shared = Entities->RegisterParticipant(NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage()),
		ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("the shared entity takes the ensure branch"), Model->IsAuthoritativeToCreateForTest(Shared))
		|| !TestFalse(TEXT("the earlier entity takes the read branch"), Model->IsAuthoritativeToCreateForTest(BoundBefore)))
	{
		return false;
	}

	SetNetStatsTestApiContext(Model);
	Model->ResolveOrCreateContainer(BoundBefore, TEXT("NetStatsType"), FString(), nullptr);
	Model->ResolveOrCreateContainer(Shared, TEXT("NetStatsType"), FString(), nullptr);
	TestEqual(TEXT("a resolve whose client cannot be built never goes out, so it is not counted"), Stats.ResolveStarts, 0);
	TestEqual(TEXT("nor counted as a re-resolve"), Stats.ReResolves, 0);

	FCrowdyGameModelTestClientHost ClientHost(Model, NetStatsTestEndpoint);
	Model->ResolveOrCreateContainer(BoundBefore, TEXT("NetStatsType"), FString(), nullptr);
	TestEqual(TEXT("the read was sent"), ClientHost.Client->NumPendingRequests(), 1);
	TestEqual(TEXT("the resolve started"), Stats.ResolveStarts, 1);
	TestEqual(TEXT("an entity bound earlier in this world counts as a re-resolve"), Stats.ReResolves, 1);

	Model->ResolveOrCreateContainer(FGuid::NewGuid(), TEXT("NetStatsType"), FString(), nullptr);
	TestEqual(TEXT("a never-bound entity starts a resolve"), Stats.ResolveStarts, 2);
	TestEqual(TEXT("but is not a re-resolve"), Stats.ReResolves, 1);

	Model->ResolveOrCreateContainer(Shared, TEXT("NetStatsType"), FString(), nullptr);
	TestEqual(TEXT("the ensure was sent"), ClientHost.Client->NumPendingRequests(), 3);
	TestEqual(TEXT("and counted once it was"), Stats.ResolveStarts, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsResetKeepsEverBoundTest,
	"CrowdySDK.GameModel.NetStats.ResetKeepsEverBound", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsResetKeepsEverBoundTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}
	SetNetStatsTestApiContext(Model);
	FCrowdyGameModelTestClientHost ClientHost(Model, NetStatsTestEndpoint);
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();

	const FGuid NetID = FGuid::NewGuid();
	Model->BindEntityContainer(NetID, TEXT("c1"));
	Model->MarkSelfActed(TEXT("c1"));
	Model->ResetNetStats();
	TestEqual(TEXT("reset zeroes the counters"), Stats.SelfInvokesMarked, 0);
	TestTrue(TEXT("reset keeps the entities bound so far"), Stats.EverBound.Contains(NetID));

	Model->UnbindEntityContainerForTest(NetID);
	Model->ResolveOrCreateContainer(NetID, TEXT("NetStatsType"), FString(), nullptr);
	TestEqual(TEXT("a resolve after the reset still sees the earlier bind"), Stats.ReResolves, 1);

	TArray<FString> Lines;
	Model->DescribeNetStats(Lines);
	TestTrue(TEXT("the description leads with the summary line"),
		Lines.Num() > 0 && Lines[0].StartsWith(TEXT("[GameModel] net stats over ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsHelpersTest,
	"CrowdySDK.GameModel.NetStats.Helpers", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsHelpersTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("no samples is zero"), FCrowdyGameModelNetStats::Percentile(TArray<double>(), 0.5), 0.0);

	TArray<double> Samples;
	for (int32 Value = 20; Value >= 1; --Value)
	{
		Samples.Add(static_cast<double>(Value));
	}
	TestEqual(TEXT("p50 is the nearest rank of a sorted copy"), FCrowdyGameModelNetStats::Percentile(Samples, 0.5), 10.0);
	TestEqual(TEXT("p95 of 1..20"), FCrowdyGameModelNetStats::Percentile(Samples, 0.95), 19.0);
	TestEqual(TEXT("p100 is the max"), FCrowdyGameModelNetStats::Percentile(Samples, 1.0), 20.0);
	TestEqual(TEXT("p0 is the min"), FCrowdyGameModelNetStats::Percentile(Samples, 0.0), 1.0);

	TestEqual(TEXT("thrown budget refusal key"),
		FCrowdyGameModelNetStats::MakeInvokeFaultKey(TEXT("RATE_LIMITED"), TEXT("BUDGET"), true, false),
		FString(TEXT("RATE_LIMITED blame=BUDGET retryable=1 thrown")));
	TestEqual(TEXT("in-band fault with no code"),
		FCrowdyGameModelNetStats::MakeInvokeFaultKey(FString(), TEXT("AUTHOR"), false, true),
		FString(TEXT("none blame=AUTHOR retryable=0 in-band")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsBoundedKeysTest,
	"CrowdySDK.GameModel.NetStats.BoundedKeys", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsBoundedKeysTest::RunTest(const FString& Parameters)
{
	using FStats = FCrowdyGameModelNetStats;
	FStats Stats;

	constexpr int32 ExtraFaults = 9;
	for (int32 Index = 0; Index < FStats::MaxInvokeFaultKeys + ExtraFaults; ++Index)
	{
		const FString Code = FString::Printf(TEXT("SERVER_CODE_%d"), Index);
		FStats::CountBounded(Stats.InvokeFaults, FStats::MakeInvokeFaultKey(Code, TEXT("PLATFORM"), true, false),
			FStats::MaxInvokeFaultKeys);
	}
	TestEqual(TEXT("distinct fault codes past the cap add only the overflow key"), Stats.InvokeFaults.Num(),
		FStats::MaxInvokeFaultKeys + 1);
	const int32* FaultOverflow = Stats.InvokeFaults.Find(FString(FStats::OverflowKey));
	TestEqual(TEXT("every fault code past the cap is counted under the overflow key"), FaultOverflow ? *FaultOverflow : 0,
		ExtraFaults);

	const FString FirstKey = FStats::MakeInvokeFaultKey(TEXT("SERVER_CODE_0"), TEXT("PLATFORM"), true, false);
	FStats::CountBounded(Stats.InvokeFaults, FirstKey, FStats::MaxInvokeFaultKeys);
	const int32* First = Stats.InvokeFaults.Find(FirstKey);
	TestEqual(TEXT("a key already held keeps counting under its own name"), First ? *First : 0, 2);

	const FString LongKey = FStats::MakeInvokeFaultKey(FString::ChrN(500, TEXT('X')), FString::ChrN(500, TEXT('B')), false, true);
	TestEqual(TEXT("a long server code and blame are clipped in the key"), LongKey,
		FString::ChrN(FStats::MaxFaultKeyPartChars, TEXT('X')) + TEXT(" blame=")
			+ FString::ChrN(FStats::MaxFaultKeyPartChars, TEXT('B')) + TEXT(" retryable=0 in-band"));

	constexpr int32 ExtraContainers = 3;
	for (int32 Index = 0; Index < FStats::MaxPulledContainerKeys + ExtraContainers; ++Index)
	{
		FStats::CountBounded(Stats.PullsByContainer, FString::Printf(TEXT("container-%d"), Index),
			FStats::MaxPulledContainerKeys);
	}
	TestEqual(TEXT("distinct containers past the cap add only the overflow key"), Stats.PullsByContainer.Num(),
		FStats::MaxPulledContainerKeys + 1);
	const int32* ContainerOverflow = Stats.PullsByContainer.Find(FString(FStats::OverflowKey));
	TestEqual(TEXT("every container past the cap is counted under the overflow key"),
		ContainerOverflow ? *ContainerOverflow : 0, ExtraContainers);

	Stats.Reset(0.0);
	TestEqual(TEXT("a reset empties the fault map"), Stats.InvokeFaults.Num(), 0);
	TestEqual(TEXT("and the container map"), Stats.PullsByContainer.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsBulkReResolveTest,
	"CrowdySDK.GameModel.NetStats.BulkReResolve", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsBulkReResolveTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;
	AllowNetStatsMissingApiContext(*this);
	AllowNetStatsMissingClientHost(*this);
	AddExpectedMessagePlain(TEXT("the list failed"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 0);

	FNetStatsTestWorld Env;
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	Model->SetEntitySubsystemForTest(Entities);
	const FCrowdyGameModelNetStats& Stats = Model->GetNetStats();

	// Two shared entities of one type, as sub-participants of one Host anchor: one bound earlier in this world.
	UObject* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	UCrowdyGameModelTestComponent* SeenComponent = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	UCrowdyGameModelTestComponent* FreshComponent = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	const FGuid Seen = Entities->RegisterSubParticipant(SeenComponent, AnchorNetID);
	const FGuid Fresh = Entities->RegisterSubParticipant(FreshComponent, AnchorNetID);
	Model->BindEntityContainer(Seen, TEXT("c-seen"));
	Model->UnbindEntityContainerForTest(Seen);
	Model->AddPendingModelEntityForTest(Seen, TEXT("Node"));
	Model->AddPendingModelEntityForTest(Fresh, TEXT("Node"));
	if (!TestTrue(TEXT("both entities take the bulk path"),
		Model->IsBulkResolveEligibleForTest(Seen) && Model->IsBulkResolveEligibleForTest(Fresh)))
	{
		return false;
	}

	Model->RetryPendingModelEntitiesForTest();
	TestEqual(TEXT("the sweep formed one bulk group"), Model->GetBulkResolveDispatchCountForTest(), 1);
	TestEqual(TEXT("a list held back for want of an API context is not a resolve"), Stats.ResolveStarts, 0);
	TestEqual(TEXT("nor a re-resolve"), Stats.ReResolves, 0);

	// With no client host the list itself fails, so this proves only that dispatching a list counts nothing.
	SetNetStatsTestApiContext(Model);
	Model->RetryPendingModelEntitiesForTest();
	TestEqual(TEXT("a list is not counted per entity when it is dispatched"), Stats.ResolveStarts, 0);
	TestEqual(TEXT("nor as re-resolves"), Stats.ReResolves, 0);

	// The server's answer is stood in for: Seen is listed, Fresh is missed and goes on to its own ensure, which a
	// client now exists to send.
	FCrowdyGameModelTestClientHost ClientHost(Model, NetStatsTestEndpoint);
	FString Session;
	Model->TryGetBulkResolveDispatchSessionForTest(TEXT("Node"), Session);
	UCrowdyGameModelSubsystem::FCrowdyBulkResolveHit Hit;
	Hit.NetID = Seen;
	Hit.ContainerId = TEXT("c-seen");
	Model->FinishBulkResolveForTest(TEXT("Node"), Session, {Seen, Fresh}, {Hit});
	TestEqual(TEXT("the listed entity was bound from the list"), Model->GetBulkResolveHitCountForTest(), 1);
	TestEqual(TEXT("each entity counts once: the hit here, the miss at its ensure"), Stats.ResolveStarts, 2);
	TestEqual(TEXT("the hit bound earlier in this world is the only re-resolve"), Stats.ReResolves, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsEverBoundCapTest,
	"CrowdySDK.GameModel.NetStats.EverBoundCap", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsEverBoundCapTest::RunTest(const FString& Parameters)
{
	using FStats = FCrowdyGameModelNetStats;
	FStats Stats;

	for (int32 Index = 0; Index < FStats::MaxEverBound; ++Index)
	{
		Stats.RecordBound(FGuid(1, 0, 0, static_cast<uint32>(Index)));
	}
	TestFalse(TEXT("a set exactly at the cap is not yet full"), Stats.bEverBoundFull);

	constexpr int32 Extra = 10;
	for (int32 Index = 0; Index < Extra; ++Index)
	{
		Stats.RecordBound(FGuid(2, 0, 0, static_cast<uint32>(Index)));
	}
	TestEqual(TEXT("entities past the cap are not added"), Stats.EverBound.Num(), FStats::MaxEverBound);
	TestTrue(TEXT("and the set reports that it is full"), Stats.bEverBoundFull);
	TestTrue(TEXT("an entity already held is still found"), Stats.EverBound.Contains(FGuid(1, 0, 0, 0)));

	Stats.Reset(0.0);
	TestTrue(TEXT("a reset keeps the full flag, since it keeps the set"), Stats.bEverBoundFull);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsClientOpStatsTest,
	"CrowdySDK.GameModel.NetStats.ClientOpStats", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsClientOpStatsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	FCrowdyCppClientConfig Config;
	Config.ApiUrl = TEXT("https://game.test");
	Config.DiscoveryUrl = TEXT("https://api.test");
	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200, Config);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	IssueSessionRead(Client);
	Client->Poll();
	TArray<FCrowdyCppOpStats> Ops;
	FCrowdyCppTransportStats Transport;
	Client->GetStats(Ops, Transport);
	const FCrowdyCppOpStats* Session = FindOp(Ops, TEXT("GameModelSession"));
	if (!TestNotNull(TEXT("the operation is recorded under its name"), Session))
	{
		return false;
	}
	TestEqual(TEXT("one completed call"), Session->Calls, 1);
	TestEqual(TEXT("an answered call is not a failure"), Session->Failures, 0);
	TestEqual(TEXT("one latency sample"), Session->RecentLatenciesMs.Num(), 1);

	// Cancelled completions are failures, and the latency window stays bounded however many arrive.
	constexpr int32 Burst = FCrowdyCppClient::OpLatencySamples + 44;
	for (int32 Index = 0; Index < Burst; ++Index)
	{
		IssueSessionRead(Client);
	}
	Client->CancelAll();
	Client->GetStats(Ops, Transport);
	Session = FindOp(Ops, TEXT("GameModelSession"));
	if (!TestNotNull(TEXT("still recorded after the burst"), Session))
	{
		return false;
	}
	TestEqual(TEXT("every completion counted"), Session->Calls, Burst + 1);
	TestEqual(TEXT("every cancellation is a failure"), Session->Failures, Burst);
	TestEqual(TEXT("the latency window is bounded"), Session->RecentLatenciesMs.Num(), FCrowdyCppClient::OpLatencySamples);

	Client->ResetStats();
	Client->GetStats(Ops, Transport);
	TestEqual(TEXT("a reset leaves no operation with calls"), Ops.Num(), 0);
	TestEqual(TEXT("the canned transport counts no bytes"), Transport.ResponseBytes, static_cast<int64>(0));
	return true;
}

// Only an invoke spends the server's invoke allowance, so only an invoke may move the governor's count: a pull, a
// resolve and a list that really went out leave it where it was.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsInvokeLedgerTest,
	"CrowdySDK.GameModel.NetStats.InvokeLedgerCountsOnlyInvokes", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsInvokeLedgerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}
	SetNetStatsTestApiContext(Model);
	FCrowdyGameModelTestClientHost ClientHost(Model, NetStatsTestEndpoint);

	Model->PullContainerState(TEXT("c1"), nullptr);
	Model->ResolveOrCreateContainer(FGuid::NewGuid(), TEXT("NetStatsType"), FString(), nullptr);
	Model->ListContainers(TEXT("NetStatsType"), FString(), nullptr);
	TestEqual(TEXT("the pull, the resolve and the list were all sent"), ClientHost.Client->NumPendingRequests(), 3);
	TestEqual(TEXT("none of them is counted by the governor"), Model->GetRecentInvokeCount(), 0);
	TestEqual(TEXT("nor by the diagnostics"), Model->GetInvokesInWindow(), 0);

	FCrowdyInvokeRequest Request;
	Request.FunctionName = TEXT("net_stats_fn");
	Request.SelfContainerId = TEXT("c1");
	Model->Invoke(Request, nullptr);
	TestEqual(TEXT("the invoke was sent"), ClientHost.Client->NumPendingRequests(), 4);
	TestEqual(TEXT("the invoke is counted by the governor"), Model->GetRecentInvokeCount(), 1);
	TestEqual(TEXT("and by the diagnostics, which agree"), Model->GetInvokesInWindow(), 1);
	TestEqual(TEXT("and in the dispatched total"), Model->GetNetStats().InvokesDispatched, 1);
	return true;
}

// An invoke the client cannot even start is answered at once and never reaches the server, so it spends nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsUnsentInvokeTest,
	"CrowdySDK.GameModel.NetStats.UnsentInvokeIsNotCounted", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsUnsentInvokeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}
	SetNetStatsTestApiContext(Model);
	FCrowdyGameModelTestClientHost ClientHost(Model, NetStatsTestEndpoint);
	ClientHost.Client->Close();

	FCrowdyInvokeRequest Request;
	Request.FunctionName = TEXT("net_stats_fn");
	Request.SelfContainerId = TEXT("c1");
	bool bAnswered = false;
	bool bSucceeded = true;
	Model->Invoke(Request, [&bAnswered, &bSucceeded](FCrowdyInvokeResult Result)
	{
		bAnswered = true;
		bSucceeded = Result.bTransportOk;
	});
	TestTrue(TEXT("the invoke was answered without being sent"), bAnswered && !bSucceeded);
	TestEqual(TEXT("the governor does not count it"), Model->GetRecentInvokeCount(), 0);
	TestEqual(TEXT("nor the diagnostics"), Model->GetInvokesInWindow(), 0);
	TestEqual(TEXT("nor the dispatched total"), Model->GetNetStats().InvokesDispatched, 0);
	return true;
}

// A request handed to the HTTP module splits into an sdk and an http sample; one whose hand-off was never recorded
// adds to neither, rather than adding a zero that would read as "no wait".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsHandOffSplitTest,
	"CrowdySDK.GameModel.NetStats.HandOffSplit", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsHandOffSplitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	IssueSessionRead(Client);
	Client->Poll();
	TArray<FCrowdyCppOpStats> Ops;
	FCrowdyCppTransportStats Transport;
	Client->GetStats(Ops, Transport);
	const FCrowdyCppOpStats* Session = FindOp(Ops, TEXT("GameModelSession"));
	if (!TestNotNull(TEXT("the unstamped call is recorded"), Session))
	{
		return false;
	}
	TestEqual(TEXT("its latency is sampled"), Session->RecentLatenciesMs.Num(), 1);
	TestEqual(TEXT("an unstamped call has no sdk sample"), Session->RecentSdkMs.Num(), 0);
	TestEqual(TEXT("nor an http sample"), Session->RecentHttpMs.Num(), 0);

	// The wait before the completion is delivered comes after the hand-off, so it belongs to http, not to sdk.
	constexpr double DeliveryDelayMs = 30.0;
	Client->SetTestStampsHandOff(true);
	IssueSessionRead(Client);
	FPlatformProcess::Sleep(static_cast<float>(DeliveryDelayMs / 1000.0));
	Client->Poll();
	Client->GetStats(Ops, Transport);
	Session = FindOp(Ops, TEXT("GameModelSession"));
	if (!TestNotNull(TEXT("the stamped call is recorded"), Session))
	{
		return false;
	}
	TestEqual(TEXT("both calls' latencies are sampled"), Session->RecentLatenciesMs.Num(), 2);
	if (!TestEqual(TEXT("the stamped call has an sdk sample"), Session->RecentSdkMs.Num(), 1)
		|| !TestEqual(TEXT("and an http sample"), Session->RecentHttpMs.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("the sdk part is not negative"), Session->RecentSdkMs[0] >= 0.0);
	// Half the delay on each side: a sleep on this platform can wake a timer tick early.
	TestTrue(TEXT("the sdk part ends before the delivery wait"), Session->RecentSdkMs[0] < DeliveryDelayMs * 0.5);
	TestTrue(TEXT("the http part holds the delivery wait"), Session->RecentHttpMs[0] >= DeliveryDelayMs * 0.5);

	Client->ResetStats();
	IssueSessionRead(Client);
	Client->Poll();
	Client->GetStats(Ops, Transport);
	Session = FindOp(Ops, TEXT("GameModelSession"));
	TestTrue(TEXT("a reset empties the split along with the latencies"),
		Session && Session->RecentSdkMs.Num() == 1 && Session->RecentLatenciesMs.Num() == 1);
	return true;
}

// A failed call is counted under a short reason taken from its outcome: the server's code, else the HTTP status,
// else canceled. The reasons per operation are bounded, the rest folding into "other", and a reset clears them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsFailureReasonsTest,
	"CrowdySDK.GameModel.NetStats.FailureReasons", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsFailureReasonsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}
	FCrowdyGameModelTestClientHost ClientHost(Model, NetStatsTestEndpoint);
	const TSharedPtr<FCrowdyCppClient>& Client = ClientHost.Client;
	auto CodedError = [](const FString& Code)
	{
		return FString::Printf(TEXT("{\"errors\":[{\"message\":\"refused for entity 42\",\"extensions\":{\"code\":\"%s\"}}]}"), *Code);
	};

	// A canceled call first, then two refusals with one code, a gateway error with no body, and ten distinct codes
	// to overflow the cap: five of them find room, five do not.
	IssueSessionRead(Client);
	Client->CancelAll();
	TArray<TPair<int32, FString>> Script = {
		TPair<int32, FString>(200, CodedError(TEXT("PLATFORM_BUSY"))),
		TPair<int32, FString>(200, CodedError(TEXT("PLATFORM_BUSY"))),
		TPair<int32, FString>(503, FString()),
	};
	constexpr int32 DistinctCodes = 10;
	for (int32 Index = 0; Index < DistinctCodes; ++Index)
	{
		Script.Emplace(200, CodedError(FString::Printf(TEXT("CODE_%d"), Index)));
	}
	const int32 Requests = Script.Num();
	Client->SetTestResponseScript(MoveTemp(Script));
	for (int32 Index = 0; Index < Requests; ++Index)
	{
		IssueSessionRead(Client);
	}
	Client->Poll();

	TArray<FCrowdyGameModelOpStatsRow> Rows;
	FCrowdyGameModelTransportTotals Totals;
	Model->GetOpStats(Rows, Totals);
	if (!TestEqual(TEXT("one operation row"), Rows.Num(), 1))
	{
		return false;
	}
	const FCrowdyGameModelOpStatsRow& Row = Rows[0];
	TestEqual(TEXT("every call failed"), Row.Failures, Requests + 1);
	auto CountOf = [&Row](const TCHAR* Reason)
	{
		const TPair<FString, int32>* Found = Row.FailureReasons.FindByPredicate(
			[Reason](const TPair<FString, int32>& Pair) { return Pair.Key == Reason; });
		return Found ? Found->Value : 0;
	};
	TestEqual(TEXT("a server refusal lands under its code"), CountOf(TEXT("PLATFORM_BUSY")), 2);
	TestEqual(TEXT("a bodiless HTTP error lands under its status"), CountOf(TEXT("http 503")), 1);
	TestEqual(TEXT("a canceled call lands under canceled"), CountOf(TEXT("canceled")), 1);
	TestEqual(TEXT("no more than eight reasons plus the overflow are kept"), Row.FailureReasons.Num(), 9);
	TestEqual(TEXT("the codes past the cap are counted under other"), CountOf(TEXT("other")), DistinctCodes - 5);
	TestEqual(TEXT("the most frequent reason comes first"), Row.FailureReasons[0].Key, FString(TEXT("other")));
	TestFalse(TEXT("no reason carries the message text"),
		Row.FailureReasons.ContainsByPredicate([](const TPair<FString, int32>& Pair) { return Pair.Key.Contains(TEXT("entity")); }));

	TArray<FString> Lines;
	Model->DescribeNetStats(Lines);
	TestTrue(TEXT("the op line prints its failures"),
		Lines.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT(" failures: other x5, PLATFORM_BUSY x2")); }));

	Client->ResetStats();
	IssueSessionRead(Client);
	Client->CancelAll();
	Model->GetOpStats(Rows, Totals);
	TestTrue(TEXT("a reset clears the reasons"),
		Rows.Num() == 1 && Rows[0].FailureReasons.Num() == 1 && Rows[0].FailureReasons[0].Key == TEXT("canceled"));
	return true;
}

// A canceled request's http time would end at the cancel rather than at an answer, so it records no split; its
// latency is still sampled.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsCanceledSplitTest,
	"CrowdySDK.GameModel.NetStats.CanceledRequestHasNoSplit", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsCanceledSplitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}
	Client->SetTestStampsHandOff(true);
	IssueSessionRead(Client);
	TestEqual(TEXT("one request was canceled"), Client->CancelAll(), 1);

	TArray<FCrowdyCppOpStats> Ops;
	FCrowdyCppTransportStats Transport;
	Client->GetStats(Ops, Transport);
	const FCrowdyCppOpStats* Session = FindOp(Ops, TEXT("GameModelSession"));
	if (!TestNotNull(TEXT("the canceled call is recorded"), Session))
	{
		return false;
	}
	TestEqual(TEXT("its latency is sampled"), Session->RecentLatenciesMs.Num(), 1);
	TestEqual(TEXT("but it has no sdk sample"), Session->RecentSdkMs.Num(), 0);
	TestEqual(TEXT("nor an http sample"), Session->RecentHttpMs.Num(), 0);
	return true;
}

// The transport counts the requests it holds open and the most it held at once since the last reset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsPeakInFlightTest,
	"CrowdySDK.GameModel.NetStats.PeakInFlight", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsPeakInFlightTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	const TSharedPtr<FCrowdyCppClient> Client = FCrowdyCppClient::MakeForTest(TEXT("{\"data\":{}}"), 200);
	if (!TestTrue(TEXT("test client constructed"), Client.IsValid()))
	{
		return false;
	}

	// A second request issued while the first is still in the transport, the overlap a burst produces.
	bool bIssuedOverlap = false;
	FCrowdyCppClient* RawClient = Client.Get();
	Client->SetTestOnRequest([RawClient, &bIssuedOverlap](const FString&)
	{
		if (bIssuedOverlap)
		{
			return;
		}
		bIssuedOverlap = true;
		RawClient->RunOp(ECrowdyCppApiDomain::GameModel, TEXT("GameModelSession"), MakeShared<FJsonObject>(),
			[](FCrowdyCppJsonResult) {});
	});
	IssueSessionRead(Client);
	Client->SetTestOnRequest(nullptr);
	Client->Poll();

	TArray<FCrowdyCppOpStats> Ops;
	FCrowdyCppTransportStats Transport;
	Client->GetStats(Ops, Transport);
	TestTrue(TEXT("the overlapping request was issued"), bIssuedOverlap);
	TestEqual(TEXT("nothing is in flight once both answered"), Transport.InFlight, 0);
	TestEqual(TEXT("two were in flight at once"), Transport.PeakInFlight, 2);

	Client->ResetStats();
	Client->GetStats(Ops, Transport);
	TestEqual(TEXT("a reset restarts the peak from what is in flight now"), Transport.PeakInFlight, 0);

	IssueSessionRead(Client);
	Client->Poll();
	Client->GetStats(Ops, Transport);
	TestEqual(TEXT("one request alone peaks at one"), Transport.PeakInFlight, 1);
	return true;
}

// The subsystem's rows carry the split as percentiles, zero when no call recorded its hand-off, and the description
// prints it along with the in-flight line.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNetStatsOpRowSplitTest,
	"CrowdySDK.GameModel.NetStats.OpRowSplit", CrowdyNetStatsTestSupport::NetStatsTestFlags)
bool FCrowdyGameModelNetStatsOpRowSplitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyNetStatsTestSupport;

	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem created"), Model))
	{
		return false;
	}
	FCrowdyGameModelTestClientHost ClientHost(Model, NetStatsTestEndpoint);

	IssueSessionRead(ClientHost.Client);
	ClientHost.Client->Poll();
	TArray<FCrowdyGameModelOpStatsRow> Rows;
	FCrowdyGameModelTransportTotals Totals;
	Model->GetOpStats(Rows, Totals);
	if (!TestEqual(TEXT("one operation row"), Rows.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("no recorded hand-off leaves the sdk p50 at zero"), Rows[0].SdkP50Ms, 0.0);
	TestEqual(TEXT("and the http p95"), Rows[0].HttpP95Ms, 0.0);

	ClientHost.Client->SetTestStampsHandOff(true);
	IssueSessionRead(ClientHost.Client);
	ClientHost.Client->Poll();
	Model->GetOpStats(Rows, Totals);
	if (!TestEqual(TEXT("still one operation row"), Rows.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("a recorded hand-off gives the http percentiles a value"), Rows[0].HttpP50Ms > 0.0 && Rows[0].HttpP95Ms > 0.0);
	TestEqual(TEXT("the peak in flight reaches the totals"), Totals.PeakInFlight, 1);

	TArray<FString> Lines;
	Model->DescribeNetStats(Lines);
	TestTrue(TEXT("the op line prints the split"),
		Lines.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("sdk p50")) && Line.Contains(TEXT("http p50")); }));
	TestTrue(TEXT("the transport in-flight line is printed"),
		Lines.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("[GameModel] transport in flight 0 peak 1")); }));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
