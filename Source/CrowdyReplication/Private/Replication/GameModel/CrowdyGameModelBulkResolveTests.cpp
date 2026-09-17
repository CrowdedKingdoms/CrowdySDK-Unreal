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

// A Host-owned participant is bulk-eligible; a locally-owned one binds its own row and is not.
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

	TestTrue(TEXT("a Host-owned entity is bulk-eligible"), Model->IsBulkResolveEligibleForTest(SharedNetID));
	TestFalse(TEXT("a locally-owned entity is not"), Model->IsBulkResolveEligibleForTest(OwnNetID));
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
// resolve on them, while a locally-owned entity still takes its own ensure. Mutating away the BulkHandled skip in
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
	TestEqual(TEXT("only the locally-owned entity took a per-entity resolve"), Model->GetContainerResolveStartCountForTest(), 1);
	for (const FGuid& NetID : SharedNetIDs)
	{
		TestEqual(TEXT("a shared entity was never asked about one by one"), Model->GetPendingRetryAttemptsForTest(NetID), 0);
	}
	TestEqual(TEXT("the locally-owned entity was"), Model->GetPendingRetryAttemptsForTest(OwnNetID), 1);
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

#endif
