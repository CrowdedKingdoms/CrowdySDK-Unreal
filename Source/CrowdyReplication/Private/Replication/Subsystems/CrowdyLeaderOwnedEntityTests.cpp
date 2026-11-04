#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Misc/AutomationTest.h"
#include "Data/CrowdyActorPoolPolicy.h"
#include "Data/CrowdyEntityTypes.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Utils/UCrowdyClassRegistry.h"

// An entity Owned By Leader follows the Leader: its owner comes from the host store, on registration and on every
// change, and only that owner's state is taken.
namespace
{
	constexpr EAutomationTestFlags CrowdyLeaderOwnedTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	struct FCrowdyLeaderEntityTestWorld
	{
		UWorld* World = nullptr;

		FCrowdyLeaderEntityTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyLeaderEntityTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	// Follows Session the way a live subsystem does.
	UCrowdyEntitySubsystem* MakeFollower(const FGuid& LocalPlayer, UCrowdyGameSession* Session)
	{
		UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(LocalPlayer);
		Entities->SetGameSessionForTest(Session);
		Entities->BindGameSessionForTest();
		return Entities;
	}

	FGuid RegisterLeaderOwned(UCrowdyEntitySubsystem* Entities, UObject* Participant)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = FGuid::NewGuid();
		Record.Participant = Participant;
		Entities->MakeLeaderOwned(Record);
		Entities->RegisterEntity(Record);
		return Record.NetID;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderOwnedRegistrationTest,
	"CrowdySDK.LeaderOwned.TakesItsOwnerFromTheLeader", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderOwnedRegistrationTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();

	struct FCase { const TCHAR* What; FGuid Leader; ECrowdyRole Role; };
	const FCase Cases[] =
	{
		{ TEXT("another player leads"), Other, ECrowdyRole::RemoteProxy },
		{ TEXT("this client leads"), Local, ECrowdyRole::Owner },
		{ TEXT("nobody leads yet"), FGuid(), ECrowdyRole::RemoteProxy },
	};

	for (const FCase& Case : Cases)
	{
		UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);
		Entities->SetGameSessionForTest(MakeGameSession(Case.Leader));

		ACrowdyLeaderOwnedTestActor* Actor = TestWorld.World->SpawnActor<ACrowdyLeaderOwnedTestActor>();
		if (!TestNotNull(TEXT("the actor spawned"), Actor))
		{
			return false;
		}
		const FGuid NetID = FGuid::NewGuid();
		Actor->Entity->SetCollaboratorsForTest(Entities, nullptr);
		Actor->Entity->AssignPooledIdentity(NetID, FGuid(), ECrowdyRole::RemoteProxy, CROWDY_INVALID_CLASS_ID);
		Actor->DispatchBeginPlay();

		const FCrowdyEntityRecord* Record = Entities->FindRecord(NetID);
		if (!TestNotNull(Case.What, Record))
		{
			return false;
		}
		TestTrue(FString::Printf(TEXT("%s: the record is Leader-owned"), Case.What), Record->bLeaderOwned);
		TestEqual(FString::Printf(TEXT("%s: the record names the Leader"), Case.What), Record->OwnerID, Case.Leader);
		TestTrue(FString::Printf(TEXT("%s: the record has the role"), Case.What), Record->Role == Case.Role);
		TestEqual(FString::Printf(TEXT("%s: the component agrees on the owner"), Case.What), Actor->Entity->GetOwnerID(), Case.Leader);
		TestTrue(FString::Printf(TEXT("%s: and on the role"), Case.What), Actor->Entity->GetRole() == Case.Role);
	}

	ECrowdyRole Role = ECrowdyRole::None;
	FGuid Owner = Other;
	UCrowdyEntityComponent::DeriveOwner(ECrowdyOwnedBy::Leader, Local, Role, Owner);
	TestTrue(TEXT("a Leader entity is no World entity before it registers"), Role == ECrowdyRole::RemoteProxy);
	TestFalse(TEXT("and has no owner until then"), Owner.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderOwnedFollowsTheLeaderTest,
	"CrowdySDK.LeaderOwned.FollowsTheLeader", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderOwnedFollowsTheLeaderTest::RunTest(const FString& Parameters)
{
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();
	const FGuid PlayerOwner = FGuid::NewGuid();
	UCrowdyGameSession* Session = MakeGameSession(Other);
	UCrowdyEntitySubsystem* Entities = MakeFollower(Local, Session);

	const FGuid LeaderOwned = RegisterLeaderOwned(Entities, NewObject<ACrowdyStateSendTestActor>());

	FCrowdyEntityRecord Plain;
	Plain.NetID = FGuid::NewGuid();
	Plain.OwnerID = PlayerOwner;
	Plain.Role = ECrowdyRole::RemoteProxy;
	Plain.Participant = NewObject<ACrowdyStateSendTestActor>();
	Entities->RegisterEntity(Plain);

	Session->SetHostID(Local);
	const FCrowdyEntityRecord* Record = Entities->FindRecord(LeaderOwned);
	TestEqual(TEXT("this client took the lead, so it owns the entity"), Record->OwnerID, Local);
	TestTrue(TEXT("and writes it"), Entities->IsLocallyOwned(LeaderOwned));

	Session->SetHostID(FGuid());
	Record = Entities->FindRecord(LeaderOwned);
	TestFalse(TEXT("no Leader: no owner"), Record->OwnerID.IsValid());
	TestTrue(TEXT("and it does not turn into a World entity"), Record->Role == ECrowdyRole::RemoteProxy);

	Session->SetHostID(Other);
	Record = Entities->FindRecord(LeaderOwned);
	TestEqual(TEXT("the next Leader owns it"), Record->OwnerID, Other);
	TestFalse(TEXT("and this client no longer writes it"), Entities->IsLocallyOwned(LeaderOwned));

	TestEqual(TEXT("a Player entity keeps its owner through all of it"), Entities->FindRecord(Plain.NetID)->OwnerID, PlayerOwner);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderOwnedLearnsOwnIdLateTest,
	"CrowdySDK.LeaderOwned.LearnsItLeadsWhenItLearnsItsId", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderOwnedLearnsOwnIdLateTest::RunTest(const FString& Parameters)
{
	// A placed entity registers before this client's avatar sets its id, and the Leader may already be known.
	const FGuid Local = FGuid::NewGuid();
	UCrowdyGameSession* Session = MakeGameSession(Local);
	UCrowdyEntitySubsystem* Entities = MakeFollower(FGuid(), Session);

	const FGuid NetID = RegisterLeaderOwned(Entities, NewObject<ACrowdyStateSendTestActor>());
	TestTrue(TEXT("before it knows its id, this client is a proxy"), Entities->FindRecord(NetID)->Role == ECrowdyRole::RemoteProxy);

	Session->SetUUID(Local.ToString(EGuidFormats::Digits));

	TestTrue(TEXT("the role follows though the owner id did not change"), Entities->FindRecord(NetID)->Role == ECrowdyRole::Owner);
	TestTrue(TEXT("so this client writes it"), Entities->IsLocallyOwned(NetID));

	// The Blueprint-callable way of setting the id reaches the same answer.
	UCrowdyEntitySubsystem* Manual = MakeEntitySubsystem(FGuid());
	Manual->SetGameSessionForTest(MakeGameSession(Local));
	const FGuid ManualID = RegisterLeaderOwned(Manual, NewObject<ACrowdyStateSendTestActor>());
	Manual->SetLocalPlayerID(Local);
	TestTrue(TEXT("Set Local Player ID does too"), Manual->IsLocallyOwned(ManualID));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderOwnedRefusesGrantsTest,
	"CrowdySDK.LeaderOwned.RefusesGrants", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderOwnedRefusesGrantsTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);
	Entities->SetGameSessionForTest(MakeGameSession(Local));

	ACrowdyLeaderOwnedTestActor* Actor = TestWorld.World->SpawnActor<ACrowdyLeaderOwnedTestActor>();
	if (!TestNotNull(TEXT("the actor spawned"), Actor))
	{
		return false;
	}
	const FGuid NetID = FGuid::NewGuid();
	Actor->Entity->SetCollaboratorsForTest(Entities, nullptr);
	Actor->Entity->AssignPooledIdentity(NetID, FGuid(), ECrowdyRole::RemoteProxy, CROWDY_INVALID_CLASS_ID);
	Actor->DispatchBeginPlay();

	AddExpectedMessage(TEXT("it is Owned By Leader"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Actor->Entity->GrantOwnershipTo(Other);
	TestEqual(TEXT("a grant from this client does not move it"), Entities->FindRecord(NetID)->OwnerID, Local);

	Actor->Entity->GrantOwnership_Implementation(Other, Local);
	TestEqual(TEXT("nor does one arriving from the network"), Entities->FindRecord(NetID)->OwnerID, Local);

	Actor->Entity->GrantOwnership_Implementation(FGuid(), Local);
	TestTrue(TEXT("and it cannot be turned into a World entity"), Entities->FindRecord(NetID)->Role == ECrowdyRole::Owner);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderOwnedFenceTest,
	"CrowdySDK.LeaderOwned.TakesStateOnlyFromItsOwner", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderOwnedFenceTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	const FCrowdyRepLayout* Layout = Registry ? Registry->FindRepLayout(ACrowdyStateSendTestActor::StaticClass()) : nullptr;
	const int32 RepIntIndex = Layout ? IndexOfPropertyName(*Layout, TEXT("RepInt")) : INDEX_NONE;
	if (!TestTrue(TEXT("the fixture has RepInt in its layout"), RepIntIndex != INDEX_NONE))
	{
		return false;
	}

	const FGuid Local = FGuid::NewGuid();
	const FGuid Leader = FGuid::NewGuid();

	// A fresh receiver for each case, so a value applied by one cannot be mistaken for the next one's.
	auto Receive = [&](const FGuid& CurrentLeader, const FGuid& Sender, const uint8 Flags, int64& OutDropped) -> int32
	{
		UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);
		Entities->SetGameSessionForTest(MakeGameSession(CurrentLeader));
		ACrowdyStateSendTestActor* Target = NewObject<ACrowdyStateSendTestActor>();
		const FGuid NetID = RegisterLeaderOwned(Entities, Target);
		UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

		ACrowdyStateSendTestActor* Source = NewObject<ACrowdyStateSendTestActor>();
		Source->RepInt = 42;
		TBitArray<> Dirty;
		Dirty.Init(false, Layout->Properties.Num());
		Dirty[RepIntIndex] = true;
		TArray<uint8> Blob;
		FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

		FCrowdyStateDelta Delta;
		Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(ACrowdyStateSendTestActor::StaticClass()));
		Delta.EntityID = NetID;
		Delta.SenderID = Sender;
		Delta.LayoutHash = Layout->LayoutHash;
		Delta.Blob = Blob;
		Delta.Flags = Flags;
		Router->DispatchEvent(MakeInboundStateEvent(Delta));

		OutDropped = Router->GetStateParticipantStats().DroppedNotFromLeader;
		return Target->RepInt;
	};

	int64 Dropped = 0;
	TestEqual(TEXT("the owner's delta is applied"), Receive(Leader, Leader, 0, Dropped), 42);
	TestEqual(TEXT("and nothing was refused"), Dropped, static_cast<int64>(0));

	TestEqual(TEXT("anyone else's is not"), Receive(Leader, FGuid::NewGuid(), 0, Dropped), 0);
	TestEqual(TEXT("and the refusal is counted"), Dropped, static_cast<int64>(1));

	TestEqual(TEXT("an old Leader's host-sourced delta does not reach the new one"),
		Receive(Local, Leader, CrowdyStateDeltaFlags::HostSourced, Dropped), 0);
	TestEqual(TEXT("and it is the Leader check that refused it"), Dropped, static_cast<int64>(1));

	TestEqual(TEXT("with no Leader known yet, a late joiner still takes state"), Receive(FGuid(), Leader, 0, Dropped), 42);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderOwnedQuietTakeoverTest,
	"CrowdySDK.LeaderOwned.NewLeaderSendsOnlyWhatItWrites", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderOwnedQuietTakeoverTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();
	UCrowdyGameSession* Session = MakeGameSession(Other);
	UCrowdyEntitySubsystem* Entities = MakeFollower(Local, Session);

	UCrowdyStateReplicator* Replicator = MakeReplicator(Registry, Local);
	Replicator->SetEntitySubsystemForTest(Entities);
	Replicator->SetKeyframeIntervalForTest(0.f);
	Replicator->SetTimeForTest(100.0);

	TArray<FCrowdyStateDelta> Sent;
	Replicator->DispatchHookForTests = [&Sent](const FCrowdyStateDelta& Delta, bool /*bTargeted*/) { Sent.Add(Delta); };

	// The new Leader may be far away or late, so what it holds can be stale.
	ACrowdyStateSendTestActor* Held = NewObject<ACrowdyStateSendTestActor>(GetTransientPackage());
	Held->RepInt = 7;
	const FGuid NetID = RegisterLeaderOwned(Entities, Held);
	Session->SetHostID(Local);
	Replicator->HandleOwnershipChangedForTest(nullptr, NetID, Local, Other);
	if (!TestTrue(TEXT("the new Leader tracks the entity"), Replicator->IsTrackedForTest(NetID)))
	{
		return false;
	}

	// An ordinary transfer as the control: a new owner's values go out as they always have.
	ACrowdyStateSendTestActor* Transferred = NewObject<ACrowdyStateSendTestActor>(GetTransientPackage());
	Transferred->RepInt = 7;
	FCrowdyEntityRecord Plain;
	Plain.NetID = FGuid::NewGuid();
	Plain.OwnerID = Other;
	Plain.Role = ECrowdyRole::RemoteProxy;
	Plain.Participant = Transferred;
	Entities->RegisterEntity(Plain);
	Entities->ReassignOwnership(Plain.NetID, Local, Other);
	Replicator->HandleOwnershipChangedForTest(nullptr, Plain.NetID, Local, Other);

	auto SentFor = [&Sent](const FGuid& ID)
	{
		return Sent.FilterByPredicate([&ID](const FCrowdyStateDelta& Delta) { return Delta.EntityID == ID; });
	};

	Replicator->RunReplicationLoopForTest();
	Replicator->SetTimeForTest(103.0);
	Replicator->RunReplicationLoopForTest();
	TestEqual(TEXT("the value the new Leader holds does not go out"), SentFor(NetID).Num(), 0);
	TestEqual(TEXT("an ordinary new owner's does"), SentFor(Plain.NetID).Num(), 1);

	Held->RepInt = 8;
	Replicator->RunReplicationLoopForTest();
	const TArray<FCrowdyStateDelta> LeaderDeltas = SentFor(NetID);
	if (!TestEqual(TEXT("what it writes does"), LeaderDeltas.Num(), 1))
	{
		return false;
	}

	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(ACrowdyStateSendTestActor::StaticClass());
	ACrowdyStateSendTestActor* Probe = NewObject<ACrowdyStateSendTestActor>(GetTransientPackage());
	TArray<int32> Changed;
	TArray<int32> Present;
	TestTrue(TEXT("it decodes"), FCrowdyStateCodec::Decode(*Layout, LeaderDeltas[0].LayoutHash, LeaderDeltas[0].Blob, Probe, Changed, &Present));
	TestEqual(TEXT("and carries that one value"), Present.Num(), 1);
	TestEqual(TEXT("which is the new one"), Probe->RepInt, 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderOwnedSpawnRefusedTest,
	"CrowdySDK.LeaderOwned.OnlyTheLeaderSpawnsOne", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderOwnedSpawnRefusedTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	const FGuid Local = FGuid::NewGuid();

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(TestWorld.World);
	Entities->SetLocalPlayerID(Local);
	Entities->SetGameSessionForTest(MakeGameSession(FGuid::NewGuid()));
	TestNull(TEXT("a client that does not lead is refused"),
		Entities->SpawnEntity(ACrowdyLeaderOwnedTestActor::StaticClass(), FTransform::Identity, FInstancedStruct()));

	Entities->SetGameSessionForTest(MakeGameSession(FGuid()));
	TestNull(TEXT("and so is every client while nobody leads"),
		Entities->SpawnEntity(ACrowdyLeaderOwnedTestActor::StaticClass(), FTransform::Identity, FInstancedStruct()));

	Entities->SetGameSessionForTest(MakeGameSession(Local));
	AActor* Spawned = Entities->SpawnEntity(ACrowdyLeaderOwnedTestActor::StaticClass(), FTransform::Identity, FInstancedStruct());
	TestNotNull(TEXT("the Leader may spawn it"), Spawned);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnerSendsUnderItsOwnIdTest,
	"CrowdySDK.LeaderOwned.EachOwnerSendsUnderItsOwnId", CrowdyLeaderOwnedTestFlags)
bool FCrowdyOwnerSendsUnderItsOwnIdTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);
	UCrowdyGameSession* Session = MakeGameSession(Local);
	Entities->SetGameSessionForTest(Session);

	ACrowdyLeaderOwnedTestActor* LeaderActor = TestWorld.World->SpawnActor<ACrowdyLeaderOwnedTestActor>();
	ACrowdyStateHostOverrideActor* PlayerActor = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("the Leader entity spawned"), LeaderActor) || !TestNotNull(TEXT("the Player entity spawned"), PlayerActor))
	{
		return false;
	}

	const FGuid LeaderEntity = FGuid::NewGuid();
	LeaderActor->Entity->SetCollaboratorsForTest(Entities, nullptr);
	LeaderActor->Entity->AssignPooledIdentity(LeaderEntity, FGuid(), ECrowdyRole::RemoteProxy, CROWDY_INVALID_CLASS_ID);
	LeaderActor->DispatchBeginPlay();
	TestEqual(TEXT("a Leader sends under its own id from the start"), LeaderActor->Entity->GetReplicationUUID(),
		FCrowdyActorId::ForOwner(LeaderEntity, Local).ToString());

	const FGuid PlayerEntity = FGuid::NewGuid();
	PlayerActor->Entity->SetCollaboratorsForTest(Entities, nullptr);
	PlayerActor->Entity->AssignPooledIdentity(PlayerEntity, Local, ECrowdyRole::Owner, CROWDY_INVALID_CLASS_ID);
	PlayerActor->DispatchBeginPlay();
	TestEqual(TEXT("a Player entity starts under its NetID"), PlayerActor->Entity->GetReplicationUUID(), PlayerEntity.ToString(EGuidFormats::Digits));

	Entities->ReassignOwnership(PlayerEntity, Other, Local);
	Entities->ReassignOwnership(PlayerEntity, Local, Other);
	TestEqual(TEXT("and moves to its owner's once it changes hands"), PlayerActor->Entity->GetReplicationUUID(),
		FCrowdyActorId::ForOwner(PlayerEntity, Local).ToString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGrantReachesAClientThatMissedTheSpawnTest,
	"CrowdySDK.Entity.GrantReachesAClientThatMissedTheSpawn", CrowdyLeaderOwnedTestFlags)
bool FCrowdyGrantReachesAClientThatMissedTheSpawnTest::RunTest(const FString& Parameters)
{
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);

	FCrowdyEntityRecord Unknown;
	Unknown.NetID = FGuid::NewGuid();
	Unknown.Role = ECrowdyRole::RemoteProxy;
	Unknown.Participant = NewObject<ACrowdyStateSendTestActor>();
	Entities->RegisterEntity(Unknown);

	Entities->ReassignOwnership(Unknown.NetID, Local, Other);
	TestTrue(TEXT("a client that never learned the owner still takes the grant"), Entities->IsLocallyOwned(Unknown.NetID));

	Entities->ReassignOwnership(Unknown.NetID, Other, FGuid::NewGuid());
	TestEqual(TEXT("a client that knows the owner still refuses a stale one"), Entities->FindRecord(Unknown.NetID)->OwnerID, Local);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPooledActorSendsOnceOwnedTest,
	"CrowdySDK.Entity.APooledActorSendsOnceItIsOwned", CrowdyLeaderOwnedTestFlags)
bool FCrowdyPooledActorSendsOnceOwnedTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);

	ACrowdyStateHostOverrideActor* Pooled = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("the pool actor spawned"), Pooled))
	{
		return false;
	}
	Pooled->Entity->Mode = ECrowdyEntityMode::Dynamic;
	Pooled->Entity->SetCollaboratorsForTest(Entities, nullptr);
	Pooled->Entity->MarkPooledDormant();
	Pooled->DispatchBeginPlay();

	FCrowdyEntityRecord Record;
	Record.NetID = FGuid::NewGuid();
	Record.OwnerID = Other;
	Record.Role = ECrowdyRole::RemoteProxy;
	Record.Participant = Pooled;
	Entities->RegisterEntity(Record);
	Pooled->Entity->AssignPooledIdentity(Record.NetID, Other, ECrowdyRole::RemoteProxy, CROWDY_INVALID_CLASS_ID);

	Entities->ReassignOwnership(Record.NetID, Local, Other);
	TestTrue(TEXT("it took ownership"), Pooled->Entity->IsLocallyOwned());
	TestTrue(TEXT("and has a state to send"), Pooled->Entity->GetReplicatedState().IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWriterTakesItsNewIdTest,
	"CrowdySDK.Entity.AClientThatStaysTheWriterSendsUnderTheNewId", CrowdyLeaderOwnedTestFlags)
bool FCrowdyWriterTakesItsNewIdTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	FCrowdyScopedLeaderSource Scope(ECrowdyLeaderSource::ElectedHost);
	const FGuid Local = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);
	Entities->SetGameSessionForTest(MakeGameSession(Local));
	UCrowdyAutoReplicator* AutoReplicator = NewObject<UCrowdyAutoReplicator>(TestWorld.World);

	ACrowdyStateHostOverrideActor* Actor = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("the entity spawned"), Actor))
	{
		return false;
	}

	FCrowdyEntityRecord Record;
	Record.NetID = FGuid::NewGuid();
	Record.OwnerID = Local;
	Record.Role = ECrowdyRole::Owner;
	Record.Participant = Actor;
	Entities->RegisterEntity(Record);

	Actor->Entity->Mode = ECrowdyEntityMode::Dynamic;
	Actor->Entity->SetAutoReplicatorForTest(AutoReplicator);
	Actor->Entity->SetCollaboratorsForTest(Entities, nullptr);
	Actor->Entity->AssignPooledIdentity(Record.NetID, Local, ECrowdyRole::Owner, 0);
	Actor->Entity->StartReplication();

	// Handed to the world on the client that leads it, so the same client keeps writing.
	Entities->ReassignOwnership(Record.NetID, FGuid(), Local);
	TestTrue(TEXT("it stayed the writer"), Actor->Entity->IsLocallyOwned());
	TestEqual(TEXT("and sends under the new id"), AutoReplicator->GetSendIdForTest(Actor->Entity),
		FCrowdyActorId::ForOwner(Record.NetID, FGuid()).ToString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTakenProxyMovesAgainTest,
	"CrowdySDK.Entity.AProxyTakenOverGetsItsMovementBack", CrowdyLeaderOwnedTestFlags)
bool FCrowdyTakenProxyMovesAgainTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	const FGuid Local = FGuid::NewGuid();
	const FGuid Other = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);

	ACharacter* Character = TestWorld.World->SpawnActor<ACharacter>();
	if (!TestNotNull(TEXT("the character spawned"), Character))
	{
		return false;
	}
	UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
	UCrowdyEntityComponent* Entity = NewObject<UCrowdyEntityComponent>(Character);
	Entity->RegisterComponent();
	Entity->SetCollaboratorsForTest(Entities, nullptr);

	FCrowdyEntityRecord Record;
	Record.NetID = FGuid::NewGuid();
	Record.OwnerID = Other;
	Record.Role = ECrowdyRole::RemoteProxy;
	Record.Participant = Character;
	Entities->RegisterEntity(Record);
	Entity->AssignPooledIdentity(Record.NetID, Other, ECrowdyRole::RemoteProxy, 0);

	CrowdyActorPool::DisableProxyMovement(Character);
	TestFalse(TEXT("a proxy has no movement"), Movement->IsActive());

	Entities->ReassignOwnership(Record.NetID, Local, Other);
	TestTrue(TEXT("the new owner's is active"), Movement->IsActive());
	TestTrue(TEXT("and ticking"), Movement->IsComponentTickEnabled());
	TestTrue(TEXT("with a movement mode"), Movement->MovementMode != MOVE_None);

	Movement->SetMovementMode(MOVE_Flying);
	CrowdyActorPool::RestoreProxyMovement(Character);
	TestEqual(TEXT("a mode the game chose is left alone"), static_cast<int32>(Movement->MovementMode), static_cast<int32>(MOVE_Flying));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderIsOfferedTest,
	"CrowdySDK.LeaderOwned.TheChoiceIsShownInTheDetailsPanel", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderIsOfferedTest::RunTest(const FString& Parameters)
{
	const UEnum* OwnedBy = StaticEnum<ECrowdyOwnedBy>();
	TestFalse(TEXT("Leader is not hidden"), OwnedBy->HasMetaData(TEXT("Hidden"), OwnedBy->GetIndexByValue(static_cast<int64>(ECrowdyOwnedBy::Leader))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderTakeoverHonoursAutoRegisterTest,
	"CrowdySDK.LeaderOwned.TakingOverHonoursAutoRegister", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderTakeoverHonoursAutoRegisterTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	const FGuid Local = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);
	UCrowdyAutoReplicator* AutoReplicator = NewObject<UCrowdyAutoReplicator>(TestWorld.World);

	const auto TakeOver = [&](const bool bAutoRegister)
	{
		ACrowdyLeaderMovingTestActor* Actor = TestWorld.World->SpawnActor<ACrowdyLeaderMovingTestActor>();
		Actor->Entity->bAutoRegister = bAutoRegister;
		Actor->Entity->SetAutoReplicatorForTest(AutoReplicator);
		Actor->Entity->SetCollaboratorsForTest(Entities, nullptr);

		FCrowdyEntityRecord Record;
		Record.NetID = FGuid::NewGuid();
		Record.Participant = Actor;
		Entities->MakeLeaderOwned(Record);
		Entities->RegisterEntity(Record);
		Actor->Entity->AssignPooledIdentity(Record.NetID, Record.OwnerID, Record.Role, 0);

		Entities->ReassignOwnership(Record.NetID, Local, FGuid());
		bool bPresenceOnly = false;
		return AutoReplicator->IsRegisteredForTest(Actor->Entity, bPresenceOnly);
	};

	TestTrue(TEXT("Auto Register on: the new Leader sends"), TakeOver(true));
	TestFalse(TEXT("Auto Register off: it waits for Start Replication"), TakeOver(false));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAvatarNeverChangesHandsTest,
	"CrowdySDK.Entity.AnAvatarNeverChangesHands", CrowdyLeaderOwnedTestFlags)
bool FCrowdyAvatarNeverChangesHandsTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	const FGuid Local = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(Local);

	ACrowdyStateHostOverrideActor* Avatar = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("the avatar spawned"), Avatar))
	{
		return false;
	}
	Avatar->Entity->SetCollaboratorsForTest(Entities, nullptr);
	Avatar->Entity->AssignPooledIdentity(Local, Local, ECrowdyRole::Owner, CROWDY_INVALID_CLASS_ID);
	Avatar->DispatchBeginPlay();

	Avatar->Entity->GrantOwnership_Implementation(FGuid::NewGuid(), Local);
	TestEqual(TEXT("a grant does not move it"), Entities->FindRecord(Local)->OwnerID, Local);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderOwnedDestroyTest,
	"CrowdySDK.LeaderOwned.OnlyTheLeaderDestroysOne", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderOwnedDestroyTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	const FGuid Local = FGuid::NewGuid();

	UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(TestWorld.World);
	Entities->SetLocalPlayerID(Local);
	Entities->SetGameSessionForTest(MakeGameSession(Local));
	AActor* Actor = TestWorld.World->SpawnActor<ACrowdyLeaderOwnedTestActor>();
	if (!TestNotNull(TEXT("the actor spawned"), Actor))
	{
		return false;
	}
	const FGuid NetID = RegisterLeaderOwned(Entities, Actor);

	FCrowdyEntityDestroyEvent Event;
	Event.EntityID = NetID;
	Entities->HandleRemoteDestroyForTest(Event);
	TestNotNull(TEXT("another client's destroy does not take it from the Leader"), Entities->FindRecord(NetID));

	Entities->SetGameSessionForTest(MakeGameSession(FGuid::NewGuid()));
	Entities->SetLocalPlayerID(Local);
	AddExpectedMessage(TEXT("only the client that leads may destroy it"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Entities->DestroyEntity(Actor);
	TestNotNull(TEXT("a client that does not lead cannot destroy it"), Entities->FindRecord(NetID));

	Entities->SetGameSessionForTest(MakeGameSession(Local));
	Entities->SetLocalPlayerID(Local);
	Entities->DestroyEntity(Actor);
	TestNull(TEXT("the Leader can"), Entities->FindRecord(NetID));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderOwnedSpawnClaimsTest,
	"CrowdySDK.LeaderOwned.SpawnEventsDoNotDecideItsOwner", CrowdyLeaderOwnedTestFlags)
bool FCrowdyLeaderOwnedSpawnClaimsTest::RunTest(const FString& Parameters)
{
	FCrowdyLeaderEntityTestWorld TestWorld;
	const FGuid Local = FGuid::NewGuid();
	const FGuid Peer = FGuid::NewGuid();

	FCrowdyEntitySpawnEvent Event;
	Event.EntityID = FGuid::NewGuid();
	Event.OwnerID = Peer;
	Event.ClassID = UCrowdyClassRegistry::Get()->GetID(ACrowdyLeaderOwnedTestActor::StaticClass());
	Event.ClassPath = ACrowdyLeaderOwnedTestActor::StaticClass()->GetPathName();

	UCrowdyEntitySubsystem* Leader = NewObject<UCrowdyEntitySubsystem>(TestWorld.World);
	Leader->SetLocalPlayerID(Local);
	Leader->SetGameSessionForTest(MakeGameSession(Local));
	Leader->HandleRemoteSpawnForTest(Event);

	auto CountActors = [&TestWorld]()
	{
		int32 Count = 0;
		for (TActorIterator<ACrowdyLeaderOwnedTestActor> It(TestWorld.World); It; ++It)
		{
			++Count;
		}
		return Count;
	};
	TestEqual(TEXT("the Leader takes no entity it did not spawn"), CountActors(), 0);

	UCrowdyEntitySubsystem* Follower = NewObject<UCrowdyEntitySubsystem>(TestWorld.World);
	Follower->SetLocalPlayerID(Local);
	Follower->SetGameSessionForTest(MakeGameSession(Peer));
	Follower->HandleRemoteSpawnForTest(Event);
	TestEqual(TEXT("a client that does not lead draws it"), CountActors(), 1);

	// A repeat naming a peer while nobody leads must not make that peer the owner the fence trusts.
	UCrowdyEntitySubsystem* Waiting = MakeEntitySubsystem(Local);
	Waiting->SetGameSessionForTest(MakeGameSession(FGuid()));
	Event.EntityID = RegisterLeaderOwned(Waiting, NewObject<ACrowdyStateSendTestActor>());
	Waiting->HandleRemoteSpawnForTest(Event);
	TestFalse(TEXT("a Leader entity with no Leader stays without an owner"), Waiting->FindRecord(Event.EntityID)->OwnerID.IsValid());
	return true;
}

#endif
