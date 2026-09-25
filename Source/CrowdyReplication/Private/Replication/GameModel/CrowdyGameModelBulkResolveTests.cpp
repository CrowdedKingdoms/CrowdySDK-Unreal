#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelTestSupport.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyBulkResolveTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyGameApiCodec::FContainerRow BulkTestRow(const TCHAR* Id, const TCHAR* Key, const TCHAR* Session = TEXT(""), int64 Owner = 0)
	{
		FCrowdyGameApiCodec::FContainerRow Row;
		Row.ContainerId = Id;
		Row.BindingKey = Key;
		Row.SessionId = Session;
		Row.OwnerUserId = Owner;
		Row.TypeName = TEXT("Node");
		return Row;
	}
}

// The list variables name the type, always bound the page, and omit an empty session (the server reads that as
// every scope, which is why the rows are filtered again on the way back).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveListVariablesTest,
	"CrowdySDK.GameModel.BulkResolveListVariables", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveListVariablesTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> AppScope = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(42, TEXT("Node"), FString(), 200, 400);
	TestEqual(TEXT("appId is a string"), AppScope->GetStringField(TEXT("appId")), FString(TEXT("42")));
	TestEqual(TEXT("typeName"), AppScope->GetStringField(TEXT("typeName")), FString(TEXT("Node")));
	TestFalse(TEXT("an empty session is omitted"), AppScope->HasField(TEXT("sessionId")));
	TestEqual(TEXT("limit is a number"), AppScope->GetNumberField(TEXT("limit")), 200.0);
	TestEqual(TEXT("offset is a number"), AppScope->GetNumberField(TEXT("offset")), 400.0);

	const TSharedPtr<FJsonObject> Scoped = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(42, TEXT("Node"), TEXT("s1"), 0, -5);
	TestEqual(TEXT("a session is sent"), Scoped->GetStringField(TEXT("sessionId")), FString(TEXT("s1")));
	TestEqual(TEXT("a zero limit is still bounded"), Scoped->GetNumberField(TEXT("limit")), 1.0);
	TestEqual(TEXT("a negative offset is clamped"), Scoped->GetNumberField(TEXT("offset")), 0.0);

	// The server refuses a limit above 1,000, so the builder clamps rather than let the page be refused.
	TestEqual(TEXT("the page cap is 1000"), FCrowdyGameApiCodec::MaxContainersPerPage, 1000);
	const TSharedPtr<FJsonObject> Oversized = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(42, TEXT("Node"), FString(), 5000, 0);
	TestEqual(TEXT("an oversized limit is clamped to 1000"), Oversized->GetNumberField(TEXT("limit")), 1000.0);
	const TSharedPtr<FJsonObject> AtCap = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(42, TEXT("Node"), FString(), 1000, 0);
	TestEqual(TEXT("a limit at the cap is sent as is"), AtCap->GetNumberField(TEXT("limit")), 1000.0);

	const TSharedPtr<FJsonObject> AnyType = FCrowdyGameApiCodec::BuildListContainersByTypeVariables(42, FString(), FString(), 1000, 0);
	TestFalse(TEXT("an empty type is omitted, which lists every type"), AnyType->HasField(TEXT("typeName")));
	return true;
}

// A page parses into typed rows; a row without an id or a key is dropped; a broken envelope is a failed read.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveParseRowsTest,
	"CrowdySDK.GameModel.BulkResolveParseRows", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveParseRowsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Env = ParseObject(TEXT(
		"{\"data\":{\"gameModelContainers\":["
		"{\"containerId\":\"c1\",\"typeName\":\"Node\",\"bindingKey\":\"k1\",\"sessionId\":\"\",\"ownerUserId\":\"77\"},"
		"{\"containerId\":\"c2\",\"typeName\":\"Node\",\"bindingKey\":\"k2\",\"sessionId\":\"s1\",\"ownerUserId\":null},"
		"{\"containerId\":\"\",\"bindingKey\":\"k3\"},"
		"{\"containerId\":\"c4\"}"
		"]}}"));
	TArray<FCrowdyGameApiCodec::FContainerRow> Rows;
	TestTrue(TEXT("parses"), FCrowdyGameApiCodec::ParseContainerRowsEnvelope(Env, true, TArray<FString>(), Rows));
	TestEqual(TEXT("two usable rows"), Rows.Num(), 2);
	if (Rows.Num() == 2)
	{
		TestEqual(TEXT("row 1 id"), Rows[0].ContainerId, FString(TEXT("c1")));
		TestEqual(TEXT("row 1 key"), Rows[0].BindingKey, FString(TEXT("k1")));
		TestEqual(TEXT("row 1 owner"), Rows[0].OwnerUserId, static_cast<int64>(77));
		TestTrue(TEXT("row 1 is app-global"), Rows[0].SessionId.IsEmpty());
		TestEqual(TEXT("row 2 session"), Rows[1].SessionId, FString(TEXT("s1")));
		TestEqual(TEXT("row 2 owner null reads as zero"), Rows[1].OwnerUserId, static_cast<int64>(0));
	}

	TArray<FString> Errors;
	Errors.Add(TEXT("boom"));
	TestFalse(TEXT("a transport error is a failed read"), FCrowdyGameApiCodec::ParseContainerRowsEnvelope(Env, true, Errors, Rows));
	TestFalse(TEXT("a missing envelope is a failed read"), FCrowdyGameApiCodec::ParseContainerRowsEnvelope(nullptr, true, TArray<FString>(), Rows));
	return true;
}

// Only a row in the plan's scope whose key names a pending entity is a hit; the server's filter is never trusted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveMatchTest,
	"CrowdySDK.GameModel.BulkResolveMatch", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveMatchTest::RunTest(const FString& Parameters)
{
	const FGuid A(1, 2, 3, 4);
	const FGuid B(5, 6, 7, 8);
	TMap<FString, FGuid> NetIDByKey;
	NetIDByKey.Add(FCrowdyModelIdentity::NetIDToContainerKey(A), A);
	NetIDByKey.Add(FCrowdyModelIdentity::NetIDToContainerKey(B), B);

	TArray<FCrowdyGameApiCodec::FContainerRow> Rows;
	Rows.Add(BulkTestRow(TEXT("c-a"), *FCrowdyModelIdentity::NetIDToContainerKey(A), TEXT(""), 9));
	Rows.Add(BulkTestRow(TEXT("c-b-other-session"), *FCrowdyModelIdentity::NetIDToContainerKey(B), TEXT("s7")));
	Rows.Add(BulkTestRow(TEXT("c-stranger"), TEXT("ffffffffffffffffffffffffffffffff")));

	TArray<UCrowdyGameModelSubsystem::FCrowdyBulkResolveHit> Hits;
	UCrowdyGameModelSubsystem::MatchContainerRowsForTest(Rows, NetIDByKey, FString(), Hits);
	TestEqual(TEXT("one hit in the app scope"), Hits.Num(), 1);
	if (Hits.Num() == 1)
	{
		TestEqual(TEXT("the hit is A"), Hits[0].NetID, A);
		TestEqual(TEXT("with its row"), Hits[0].ContainerId, FString(TEXT("c-a")));
		TestEqual(TEXT("and its owner"), Hits[0].OwnerUserId, static_cast<int64>(9));
	}

	Hits.Reset();
	UCrowdyGameModelSubsystem::MatchContainerRowsForTest(Rows, NetIDByKey, TEXT("s7"), Hits);
	TestEqual(TEXT("scoped to s7 only B's row is a hit"), Hits.Num(), 1);
	TestTrue(TEXT("the s7 hit is B"), Hits.Num() == 1 && Hits[0].NetID == B);
	return true;
}

// A Host-owned participant and a remote copy of another player's entity are bulk-eligible; a locally owned
// per-player entity binds its own row and is not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveEligibilityTest,
	"CrowdySDK.GameModel.BulkResolveEligibility", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveEligibilityTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	Model->SetEntitySubsystemForTest(Entities);

	UObject* Shared = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid SharedNetID = Entities->RegisterParticipant(Shared, ECrowdyOwnership::Host);
	UObject* Own = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid OwnNetID = Entities->RegisterParticipant(Own, ECrowdyOwnership::LocalClient);
	FCrowdyEntityRecord Proxy;
	Proxy.NetID = FGuid::NewGuid();
	Proxy.OwnerID = FGuid::NewGuid();
	Proxy.Role = ECrowdyRole::RemoteProxy;
	Proxy.Participant = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	Entities->RegisterEntity(Proxy);

	TestTrue(TEXT("a Host-owned entity is bulk-eligible"), Model->IsBulkResolveEligibleForTest(SharedNetID));
	TestTrue(TEXT("a remote copy of another player's entity is bulk-eligible"), Model->IsBulkResolveEligibleForTest(Proxy.NetID));
	TestFalse(TEXT("a locally owned per-player entity is not"), Model->IsBulkResolveEligibleForTest(OwnNetID));
	TestFalse(TEXT("an unknown entity is not"), Model->IsBulkResolveEligibleForTest(FGuid::NewGuid()));
	return true;
}

namespace
{
	// Mirrors the coalesce suite's world: a subsystem outered to a real world has a timer manager to arm sweeps on.
	struct FCrowdyBulkTestWorld
	{
		UWorld* World = nullptr;

		FCrowdyBulkTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyBulkTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	void AllowMissingApiContextForBulkTests(FAutomationTestBase& Test)
	{
		Test.AddExpectedError(TEXT("Game API endpoint is empty|No UCrowdyGameSession|No app-scoped game token"),
			EAutomationExpectedErrorFlags::Contains, 0);
	}

	void BulkTestTickPastSweep(UWorld* World)
	{
		World->GetTimerManager().Tick(3.0f);
		++GFrameCounter;
		World->GetTimerManager().Tick(3.0f);
	}
}

// A sweep that leaves entities pending re-arms itself from inside its own timer callback. The timer manager still
// reports the running timer, and reading that as "already armed" left every later sweep waiting for a notification
// that a quiet session never sends.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveSweepRearmsFromItsOwnTickTest,
	"CrowdySDK.GameModel.PendingSweepRearmsFromItsOwnTick", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveSweepRearmsFromItsOwnTickTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContextForBulkTests(*this);
	FCrowdyBulkTestWorld Env;
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	Model->AddPendingModelEntityForTest(FGuid::NewGuid(), TEXT("PlayerStats"));
	Model->RequestPendingModelEntitySweepForTest();

	BulkTestTickPastSweep(Env.World);
	TestEqual(TEXT("the armed sweep ran"), Model->GetPendingSweepCountForTest(), 1);
	BulkTestTickPastSweep(Env.World);
	TestEqual(TEXT("it re-armed itself for the entity still pending"), Model->GetPendingSweepCountForTest(), 2);
	return true;
}

// The sweep hands Host-owned pending entities of one type to the bulk path as ONE group and spends no per-entity
// resolve on them, while a locally owned per-player entity still takes its own ensure. Mutating away the BulkHandled skip in
// RetryPendingModelEntities turns the second assertion red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveSweepGroupsSharedEntitiesTest,
	"CrowdySDK.GameModel.BulkResolveSweepGroupsSharedEntities", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveSweepGroupsSharedEntitiesTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContextForBulkTests(*this);
	FCrowdyBulkTestWorld Env;
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	Model->SetEntitySubsystemForTest(Entities);

	// A participant's id is one per class, so five distinct shared entities are five component sub-participants
	// of one Host anchor: each inherits HostOwned and gets its own id from its name.
	UObject* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	TArray<UObject*> Keep;
	TArray<FGuid> SharedNetIDs;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		UCrowdyGameModelTestComponent* Shared = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
		Keep.Add(Shared);
		const FGuid NetID = Entities->RegisterSubParticipant(Shared, AnchorNetID);
		SharedNetIDs.Add(NetID);
		Model->AddPendingModelEntityForTest(NetID, TEXT("Node"));
	}
	UObject* Own = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid OwnNetID = Entities->RegisterParticipant(Own, ECrowdyOwnership::LocalClient);
	Model->AddPendingModelEntityForTest(OwnNetID, TEXT("PlayerStats"));

	for (const FGuid& NetID : SharedNetIDs)
	{
		TestTrue(FString::Printf(TEXT("shared %s is eligible before the sweep"), *NetID.ToString()), Model->IsBulkResolveEligibleForTest(NetID));
	}
	TestEqual(TEXT("five distinct shared ids"), TSet<FGuid>(SharedNetIDs).Num(), 5);

	Model->RetryPendingModelEntitiesForTest();
	TestEqual(TEXT("five shared entities of one type are one bulk group"), Model->GetBulkResolveDispatchCountForTest(), 1);
	TestEqual(TEXT("only the locally owned per-player entity took a per-entity resolve"), Model->GetContainerResolveStartCountForTest(), 1);
	for (const FGuid& NetID : SharedNetIDs)
	{
		TestEqual(TEXT("a shared entity was never asked about one by one"), Model->GetPendingRetryAttemptsForTest(NetID), 0);
	}
	TestEqual(TEXT("the locally owned per-player entity was"), Model->GetPendingRetryAttemptsForTest(OwnNetID), 1);
	return true;
}

// An app-scoped type binds with no session even while a session is active; a session-scoped one takes the active
// session. Mutating the app-scoped branch of ResolveContainerSessionId to return the session turns the first red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerScopeResolvesSessionTest,
	"CrowdySDK.GameModel.ContainerScopeResolvesSession", CrowdyBulkResolveTestFlags)
bool FCrowdyContainerScopeResolvesSessionTest::RunTest(const FString& Parameters)
{
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	Model->SetActiveSession(TEXT("s-active"));
	Model->MarkContainerTypeAppScopedForTest(TEXT("Landmark"));

	TestTrue(TEXT("the marked type reads as app-scoped"), Model->IsContainerTypeAppScoped(TEXT("Landmark")));
	TestFalse(TEXT("an unrecorded type reads as session-scoped"), Model->IsContainerTypeAppScoped(TEXT("Health")));
	TestTrue(TEXT("an app-scoped type resolves to no session under an active one"),
		Model->ResolveContainerSessionId(TEXT("Landmark"), FString()).IsEmpty());
	TestTrue(TEXT("and to no session even when one is named"),
		Model->ResolveContainerSessionId(TEXT("Landmark"), TEXT("s-explicit")).IsEmpty());
	TestEqual(TEXT("a session-scoped type resolves to the active session"),
		Model->ResolveContainerSessionId(TEXT("Health"), FString()), FString(TEXT("s-active")));
	TestEqual(TEXT("and an explicit session still wins for it"),
		Model->ResolveContainerSessionId(TEXT("Health"), TEXT("s-explicit")), FString(TEXT("s-explicit")));
	return true;
}

// The sweep's bulk group for an app-scoped type lists with no session even under an active one, while a
// session-scoped type's group lists under the active session. Mutating ResolveContainerSessionId's app-scoped branch
// to return the session turns the first assertion red.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveGroupSessionFollowsScopeTest,
	"CrowdySDK.GameModel.BulkResolveGroupSessionFollowsScope", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveGroupSessionFollowsScopeTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContextForBulkTests(*this);
	FCrowdyBulkTestWorld Env;
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(Env.World);
	Model->SetEntitySubsystemForTest(Entities);
	Model->SetActiveSession(TEXT("s-active"));
	Model->MarkContainerTypeAppScopedForTest(TEXT("Landmark"));

	UObject* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	UCrowdyGameModelTestComponent* LandmarkEntity = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	UCrowdyGameModelTestComponent* NodeEntity = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	Model->AddPendingModelEntityForTest(Entities->RegisterSubParticipant(LandmarkEntity, AnchorNetID), TEXT("Landmark"));
	Model->AddPendingModelEntityForTest(Entities->RegisterSubParticipant(NodeEntity, AnchorNetID), TEXT("Node"));

	Model->RetryPendingModelEntitiesForTest();
	TestEqual(TEXT("one group per type"), Model->GetBulkResolveDispatchCountForTest(), 2);

	FString Session = TEXT("unset");
	TestTrue(TEXT("the app-scoped type's group was dispatched"), Model->TryGetBulkResolveDispatchSessionForTest(TEXT("Landmark"), Session));
	TestTrue(TEXT("and lists with no session under an active one"), Session.IsEmpty());
	TestTrue(TEXT("the session-scoped type's group was dispatched"), Model->TryGetBulkResolveDispatchSessionForTest(TEXT("Node"), Session));
	TestEqual(TEXT("and lists under the active session"), Session, FString(TEXT("s-active")));
	return true;
}

#if WITH_METADATA
// The scope is read off the class declaration: CrowdyScope="App" is app-scoped, an absent tag is not, and a subclass
// that restates nothing follows the nearest base that declares a scope.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerScopeReadsClassMetaTest,
	"CrowdySDK.GameModel.ContainerScopeReadsClassMeta", CrowdyBulkResolveTestFlags)
bool FCrowdyContainerScopeReadsClassMetaTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("CrowdyScope=App reads as app-scoped"),
		FCrowdyAttributeRegistry::IsContainerAppScoped(UCrowdyGameModelAppScopedTarget::StaticClass()));
	TestTrue(TEXT("a subclass with no scope tag of its own follows its app-scoped base"),
		FCrowdyAttributeRegistry::IsContainerAppScoped(UCrowdyGameModelAppScopedChildTarget::StaticClass()));
	TestFalse(TEXT("a container with no scope tag reads as session-scoped"),
		FCrowdyAttributeRegistry::IsContainerAppScoped(UCrowdyGameModelTestComponent::StaticClass()));
	TestFalse(TEXT("a null class reads as session-scoped"), FCrowdyAttributeRegistry::IsContainerAppScoped(nullptr));
	return true;
}
#endif

// A bulk state read costs one call per 500 ids, so a full bulk-resolve page of 1,000 hits is two state calls.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkStateChunksTest,
	"CrowdySDK.GameModel.BulkStateChunks", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkStateChunksTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("chunk size"), FCrowdyGameApiCodec::MaxContainerStatesPerCall, 500);
	TestEqual(TEXT("no ids cost no call"), UCrowdyGameModelSubsystem::ContainerStateCallsForTest(0), 0);
	TestEqual(TEXT("one id is one call"), UCrowdyGameModelSubsystem::ContainerStateCallsForTest(1), 1);
	TestEqual(TEXT("500 ids are one call"), UCrowdyGameModelSubsystem::ContainerStateCallsForTest(500), 1);
	TestEqual(TEXT("501 ids are two calls"), UCrowdyGameModelSubsystem::ContainerStateCallsForTest(501), 2);
	TestEqual(TEXT("a full 1,000-row resolve page is two calls"), UCrowdyGameModelSubsystem::ContainerStateCallsForTest(1000), 2);

	// The slicing the read sends: 1,001 ids are three calls, no call carries more than 500, the last carries the one
	// remainder in input order, and a chunk past the end is empty. Mutating the slice bound to 501 turns this red.
	TArray<FString> Ids;
	for (int32 Index = 0; Index < 1001; ++Index)
	{
		Ids.Add(FString::Printf(TEXT("c%d"), Index));
	}
	const int32 Calls = UCrowdyGameModelSubsystem::ContainerStateCallsForTest(Ids.Num());
	TestEqual(TEXT("1,001 ids are three calls"), Calls, 3);
	int32 Total = 0;
	for (int32 Chunk = 0; Chunk < Calls; ++Chunk)
	{
		TArray<FString> Slice;
		UCrowdyGameModelSubsystem::ContainerStateChunkForTest(Ids, Chunk, Slice);
		TestTrue(FString::Printf(TEXT("chunk %d carries at most 500 ids"), Chunk), Slice.Num() <= 500);
		TestTrue(FString::Printf(TEXT("chunk %d carries at least one id"), Chunk), Slice.Num() >= 1);
		TestTrue(FString::Printf(TEXT("chunk %d starts where the previous ended"), Chunk),
			Slice.Num() > 0 && Slice[0] == FString::Printf(TEXT("c%d"), Total));
		Total += Slice.Num();
	}
	TestEqual(TEXT("every id is sent exactly once"), Total, 1001);
	TArray<FString> Last;
	UCrowdyGameModelSubsystem::ContainerStateChunkForTest(Ids, 2, Last);
	TestEqual(TEXT("the last chunk carries the one remainder"), Last.Num(), 1);
	TestEqual(TEXT("and it is the last id"), Last.Num() == 1 ? Last[0] : FString(), FString(TEXT("c1000")));
	TArray<FString> Past;
	UCrowdyGameModelSubsystem::ContainerStateChunkForTest(Ids, 3, Past);
	TestEqual(TEXT("a chunk past the end is empty"), Past.Num(), 0);

	const TSharedPtr<FJsonObject> Vars = FCrowdyGameApiCodec::BuildContainerStatesVariables(42, TArray<FString>(Ids.GetData(), 500));
	TestEqual(TEXT("appId is a string"), Vars->GetStringField(TEXT("appId")), FString(TEXT("42")));
	TestEqual(TEXT("a full chunk's variables carry 500 ids"), Vars->GetArrayField(TEXT("containerIds")).Num(), 500);
	return true;
}

namespace
{
	const TCHAR* const BulkProxyEndpoint = TEXT("https://game.test");

	// A full list page of rows whose keys name no entity here.
	FString BulkProxyFullPage(int32 Page)
	{
		FString Body = TEXT("{\"data\":{\"gameModelContainers\":[");
		for (int32 Row = 0; Row < UCrowdyGameModelSubsystem::BulkResolvePageSize; ++Row)
		{
			Body += FString::Printf(TEXT("%s{\"containerId\":\"r%d-%d\",\"typeName\":\"Hero\",\"bindingKey\":\"k%d-%d\",\"sessionId\":\"\"}"),
				Row == 0 ? TEXT("") : TEXT(","), Page, Row, Page, Row);
		}
		return Body + TEXT("]}}");
	}

	// A signed-in model with a canned client that answers every call with an empty list, counting each request.
	struct FBulkProxyRig
	{
		UCrowdyEntitySubsystem* Entities = nullptr;
		UCrowdyGameModelSubsystem* Model = nullptr;
		FCrowdyGameModelTestClientHost ClientHost;
		FGuid LocalPlayer = FGuid::NewGuid();
		FGuid HostAnchor;
		TArray<UObject*> Keep;
		int32 Sends = 0;

		FBulkProxyRig()
			: Entities(NewObject<UCrowdyEntitySubsystem>(GetTransientPackage()))
			, Model(NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage()))
			, ClientHost(Model, BulkProxyEndpoint, TEXT("{\"data\":{\"gameModelContainers\":[]}}"))
		{
			Entities->SetLocalPlayerID(LocalPlayer);
			Model->SetEntitySubsystemForTest(Entities);
			Model->BeginWorldSessionForTest();
			Model->SetApiContextForTest(BulkProxyEndpoint, TEXT("test-token"), 42);
			ClientHost.Client->SetTestOnRequest([this](const FString&) { ++Sends; });
		}

		~FBulkProxyRig()
		{
			ClientHost.Client->SetTestOnRequest(nullptr);
		}

		// A pending remote copy of another player's entity.
		FGuid AddProxy()
		{
			FCrowdyEntityRecord Record;
			Record.NetID = FGuid::NewGuid();
			Record.OwnerID = FGuid::NewGuid();
			Record.Role = ECrowdyRole::RemoteProxy;
			Record.Participant = Keep.Add_GetRef(NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage()));
			Entities->RegisterEntity(Record);
			Model->AddPendingModelEntityForTest(Record.NetID, TEXT("Hero"));
			return Record.NetID;
		}

		// A pending shared entity, enrolled under one Host anchor since a participant's id is one per class.
		FGuid AddShared()
		{
			if (!HostAnchor.IsValid())
			{
				HostAnchor = Entities->RegisterParticipant(Keep.Add_GetRef(NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage())),
					ECrowdyOwnership::Host);
			}
			UCrowdyGameModelTestComponent* Shared = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
			Keep.Add(Shared);
			const FGuid NetID = Entities->RegisterSubParticipant(Shared, HostAnchor);
			Model->AddPendingModelEntityForTest(NetID, TEXT("Hero"));
			return NetID;
		}

		// The variables the last request carried: a keyed read names a binding key and no page, an ensure an input.
		TSharedPtr<FJsonObject> LastVariables() const
		{
			FString Body;
			ClientHost.Client->GetLastTestRequestBody(Body);
			const TSharedPtr<FJsonObject> Request = ParseObject(Body);
			const TSharedPtr<FJsonObject>* Variables = nullptr;
			return Request.IsValid() && Request->TryGetObjectField(TEXT("variables"), Variables) ? *Variables : MakeShared<FJsonObject>();
		}

		bool PollIdle()
		{
			return ClientHost.PollUntil([this] { return ClientHost.Client->NumPendingRequests() == 0; });
		}
	};
}

// A miss is decided by the entity's role: a remote copy the list read to its end makes no call and backs off, one a
// cut list never reached reads its own row by key, and a shared entity still ensures its row.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveMissByRoleTest,
	"CrowdySDK.GameModel.BulkResolveMissDecidedByRole", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveMissByRoleTest::RunTest(const FString& Parameters)
{
	FBulkProxyRig Rig;
	const FGuid Absent = Rig.AddProxy();
	Rig.Model->FinishBulkResolveForTest(TEXT("Hero"), FString(), {Absent}, {}, true);
	TestEqual(TEXT("a remote copy the complete list did not name makes no call"), Rig.Sends, 0);
	TestEqual(TEXT("and starts no resolve"), Rig.Model->GetContainerResolveStartCountForTest(), 0);
	TestEqual(TEXT("it backs off instead"), Rig.Model->GetPendingRetryAttemptsForTest(Absent), 1);
	FString Type;
	TestTrue(TEXT("and stays pending"), Rig.Model->TryGetPendingModelEntityTypeForTest(Absent, Type));

	const FGuid Unreached = Rig.AddProxy();
	Rig.Model->FinishBulkResolveForTest(TEXT("Hero"), FString(), {Unreached}, {}, false);
	TestEqual(TEXT("a remote copy a cut list never reached sends one call"), Rig.Sends, 1);
	const TSharedPtr<FJsonObject> Keyed = Rig.LastVariables();
	TestTrue(TEXT("its keyed read"), !Keyed->HasField(TEXT("limit"))
		&& Keyed->GetStringField(TEXT("bindingKey")) == FCrowdyModelIdentity::NetIDToContainerKey(Unreached));
	Rig.PollIdle();

	const FGuid Shared = Rig.AddShared();
	Rig.Model->FinishBulkResolveForTest(TEXT("Hero"), FString(), {Shared}, {}, true);
	TestEqual(TEXT("a shared miss on a complete list still sends one call"), Rig.Sends, 2);
	TestTrue(TEXT("its ensure"), Rig.LastVariables()->HasField(TEXT("input")));
	Rig.PollIdle();
	return true;
}

// A hit for an entity that is now owned here is not bound from the list: its own ensure binds it, which refuses a
// row another user holds. A remote copy's hit and a shared entity's hit still bind.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveHitNowOwnedHereTest,
	"CrowdySDK.GameModel.BulkResolveHitNowOwnedHereIsNotBound", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveHitNowOwnedHereTest::RunTest(const FString& Parameters)
{
	FBulkProxyRig Rig;
	const FGuid Moved = Rig.AddProxy();
	const FGuid Remote = Rig.AddProxy();
	const FGuid Shared = Rig.AddShared();
	Rig.Entities->ReassignOwnership(Moved, Rig.LocalPlayer, FGuid());
	if (!TestTrue(TEXT("ownership moved here while the list was out"), Rig.Entities->IsLocallyOwned(Moved)))
	{
		return false;
	}

	TArray<UCrowdyGameModelSubsystem::FCrowdyBulkResolveHit> Hits;
	for (const TPair<FGuid, const TCHAR*>& Listed : { TPair<FGuid, const TCHAR*>(Moved, TEXT("c-moved")),
		TPair<FGuid, const TCHAR*>(Remote, TEXT("c-remote")), TPair<FGuid, const TCHAR*>(Shared, TEXT("c-shared")) })
	{
		UCrowdyGameModelSubsystem::FCrowdyBulkResolveHit& Hit = Hits.AddDefaulted_GetRef();
		Hit.NetID = Listed.Key;
		Hit.ContainerId = Listed.Value;
	}
	Rig.Model->FinishBulkResolveForTest(TEXT("Hero"), FString(), {Moved, Remote, Shared}, Hits, true);

	FString Bound;
	TestFalse(TEXT("the entity now owned here is not bound from the list"), Rig.Model->TryGetContainerId(Moved, Bound));
	TestTrue(TEXT("a remote copy's hit binds"), Rig.Model->TryGetContainerId(Remote, Bound) && Bound == TEXT("c-remote"));
	TestTrue(TEXT("a shared entity's hit binds"), Rig.Model->TryGetContainerId(Shared, Bound) && Bound == TEXT("c-shared"));
	TestEqual(TEXT("two hits were bound"), Rig.Model->GetBulkResolveHitCountForTest(), 2);
	TestTrue(TEXT("the owned entity went on to its own ensure"), Rig.Model->GetPendingRetryAttemptsForTest(Moved) == 1);
	Rig.PollIdle();
	return true;
}

// A group with no shared entity reads at most one page per entity. A walk cut by that budget marks its type, and
// from then on that type's remote copies resolve one by one however many there are, while a shared entity still lists.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveProxyPageBudgetTest,
	"CrowdySDK.GameModel.BulkResolveProxyPageBudget", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveProxyPageBudgetTest::RunTest(const FString& Parameters)
{
	FBulkProxyRig Rig;
	Rig.ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, BulkProxyFullPage(0)),
		TPair<int32, FString>(200, BulkProxyFullPage(1)), TPair<int32, FString>(200, TEXT("{\"data\":{\"gameModelContainers\":[]}}")) });
	const FGuid First = Rig.AddProxy();
	const FGuid Second = Rig.AddProxy();

	Rig.Model->RetryPendingModelEntitiesForTest();
	TestEqual(TEXT("the two remote copies are one group"), Rig.Model->GetBulkResolveDispatchCountForTest(), 1);
	TestTrue(TEXT("every call landed"), Rig.PollIdle());
	TestEqual(TEXT("two entities read at most two pages"), Rig.Model->GetBulkResolveListCallCountForTest(), 2);
	TestTrue(TEXT("the cut walk marks its type"), Rig.Model->IsBulkResolveGroupCutForTest(TEXT("Hero"), FString()));
	TestEqual(TEXT("both misses read their own row by key"), Rig.Model->GetContainerResolveStartCountForTest(), 2);
	TestTrue(TEXT("and both back off"), Rig.Model->GetPendingRetryAttemptsForTest(First) == 1
		&& Rig.Model->GetPendingRetryAttemptsForTest(Second) == 1);

	Rig.AddProxy();
	Rig.AddProxy();
	Rig.AddProxy();
	Rig.Model->RetryPendingModelEntitiesForTest();
	TestEqual(TEXT("remote copies of a cut type are not listed"), Rig.Model->GetBulkResolveDispatchCountForTest(), 1);
	TestEqual(TEXT("no page is read for them"), Rig.Model->GetBulkResolveListCallCountForTest(), 2);
	TestEqual(TEXT("the three new ones resolve one by one, the two backing off do not"),
		Rig.Model->GetContainerResolveStartCountForTest(), 5);
	Rig.PollIdle();

	Rig.AddShared();
	Rig.Model->RetryPendingModelEntitiesForTest();
	TestEqual(TEXT("a shared entity of a cut type still lists"), Rig.Model->GetBulkResolveDispatchCountForTest(), 2);
	Rig.PollIdle();
	return true;
}

// A remote copy whose own backoff is running is kept out of the next list.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveBackoffSitsOutTest,
	"CrowdySDK.GameModel.BulkResolveBackingOffEntitySitsOutTheList", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveBackoffSitsOutTest::RunTest(const FString& Parameters)
{
	FBulkProxyRig Rig;
	const FGuid Absent = Rig.AddProxy();
	Rig.Model->FinishBulkResolveForTest(TEXT("Hero"), FString(), {Absent}, {}, true);
	TestEqual(TEXT("the complete list left it backing off"), Rig.Model->GetPendingRetryAttemptsForTest(Absent), 1);

	Rig.Model->RetryPendingModelEntitiesForTest();
	TestEqual(TEXT("the next sweep lists nothing"), Rig.Model->GetBulkResolveDispatchCountForTest(), 0);
	TestEqual(TEXT("and sends nothing"), Rig.Sends, 0);
	return true;
}

// A group with a shared entity walks past the one-page-per-entity budget: its misses ensure, so the list is cheaper.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveSharedWalkTest,
	"CrowdySDK.GameModel.BulkResolveSharedGroupWalksPastBudget", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveSharedWalkTest::RunTest(const FString& Parameters)
{
	FBulkProxyRig Rig;
	Rig.ClientHost.Client->SetTestResponseScript({ TPair<int32, FString>(200, BulkProxyFullPage(0)),
		TPair<int32, FString>(200, BulkProxyFullPage(1)) });
	Rig.AddShared();

	Rig.Model->RetryPendingModelEntitiesForTest();
	TestTrue(TEXT("every call landed"), Rig.PollIdle());
	TestEqual(TEXT("one shared entity's list reads to its short third page"), Rig.Model->GetBulkResolveListCallCountForTest(), 3);
	TestFalse(TEXT("and does not mark its type as cut"), Rig.Model->IsBulkResolveGroupCutForTest(TEXT("Hero"), FString()));
	TestEqual(TEXT("the stats count the pages"), Rig.Model->GetNetStats().BulkResolveListPages, 3);
	Rig.Model->ResetNetStats();
	TestEqual(TEXT("and a reset clears them"), Rig.Model->GetNetStats().BulkResolveListPages, 0);
	return true;
}

#if WITH_METADATA
// An entity parked for the bulk list forgets its parked scope when it unregisters.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveUnregisterForgetsSessionTest,
	"CrowdySDK.GameModel.BulkResolveUnregisterForgetsParkedSession", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveUnregisterForgetsSessionTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	Model->SetEntitySubsystemForTest(Entities);
	const FGuid Anchor = Entities->RegisterParticipant(NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage()), ECrowdyOwnership::Host);
	UCrowdyGameModelTestComponent* Shared = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	const FGuid NetID = Entities->RegisterSubParticipant(Shared, Anchor);

	Model->HandleEntityRegisteredForTest(NetID);
	TestEqual(TEXT("the shared entity is parked with its scope"), Model->GetPendingSessionCountForTest(), 1);
	Model->HandleEntityUnregisteredForTest(NetID);
	TestEqual(TEXT("unregistering forgets it"), Model->GetPendingSessionCountForTest(), 0);
	return true;
}

// A remote copy keeps no scope from its registration: its list follows the session active when the sweep runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBulkResolveRemoteCopyFollowsSessionTest,
	"CrowdySDK.GameModel.BulkResolveRemoteCopyFollowsActiveSession", CrowdyBulkResolveTestFlags)
bool FCrowdyBulkResolveRemoteCopyFollowsSessionTest::RunTest(const FString& Parameters)
{
	AllowMissingApiContextForBulkTests(*this);
	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	Entities->SetLocalPlayerID(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	Model->SetEntitySubsystemForTest(Entities);
	Model->SetActiveSession(TEXT("s-old"));
	FCrowdyEntityRecord Proxy;
	Proxy.NetID = FGuid::NewGuid();
	Proxy.OwnerID = FGuid::NewGuid();
	Proxy.Role = ECrowdyRole::RemoteProxy;
	Proxy.Participant = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	Entities->RegisterEntity(Proxy);

	Model->HandleEntityRegisteredForTest(Proxy.NetID);
	FString Type;
	TestTrue(TEXT("the remote copy waits for its type's list"), Model->TryGetPendingModelEntityTypeForTest(Proxy.NetID, Type));
	Model->SetActiveSession(TEXT("s-new"));
	Model->RetryPendingModelEntitiesForTest();

	FString Session;
	TestTrue(TEXT("its group was dispatched"), Model->TryGetBulkResolveDispatchSessionForTest(Type, Session));
	TestEqual(TEXT("under the session active now"), Session, FString(TEXT("s-new")));
	return true;
}
#endif

#endif
