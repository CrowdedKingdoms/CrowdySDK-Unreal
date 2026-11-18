// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyActorPoolBackend.h"
#include "Data/CrowdyActorPoolBackendConfig.h"
#include "Data/CrowdyEntityTypes.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/Ownership/CrowdyOwnershipTransfer.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorPoolSubsystem.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/UCrowdyClassRegistry.h"

// A spawned entity appears once on every client, with its spawn data, whichever of its spawn event and its first
// update arrives first; a destroyed one is not brought back by a late update; one that does not move keeps sending
// its state slowly so a player who joins later still sees it.
namespace
{
	constexpr EAutomationTestFlags CrowdyAppearanceTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FInstancedStruct MakeAppearanceState(const double X)
	{
		FCrowdyActorState Value;
		Value.Location = FVector(X, 0.0, 0.0);
		return FInstancedStruct::Make(Value);
	}

	double AppearanceStateX(const FInstancedStruct& State)
	{
		const FCrowdyActorState* Typed = State.GetPtr<FCrowdyActorState>();
		return Typed ? Typed->Location.X : -1.0;
	}

	// An editor world: a headless Game world trips an assert in MassEntitySubsystem, so the subsystems are built directly.
	struct FCrowdyAppearanceTestWorld
	{
		UWorld* World = nullptr;

		FCrowdyAppearanceTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyAppearanceTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	// A pool backend whose pool pre-warms one actor and may never grow past it, so a consumed pool actor shows.
	struct FCrowdyAppearanceFixture
	{
		UWorld* World = nullptr;
		UCrowdyActorPoolSubsystem* Pool = nullptr;
		UCrowdyEntitySubsystem* Entities = nullptr;
		UCrowdyActorPoolBackendConfig* Config = nullptr;
		UCrowdyActorPoolBackend* Backend = nullptr;
		TStrongObjectPtr<UCrowdySpawnDataListener> Listener;

		explicit FCrowdyAppearanceFixture(UWorld* InWorld)
			: World(InWorld)
			, Listener(NewObject<UCrowdySpawnDataListener>(GetTransientPackage()))
		{
			Pool = NewObject<UCrowdyActorPoolSubsystem>(World);
			Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());

			Config = NewObject<UCrowdyActorPoolBackendConfig>(GetTransientPackage());
			Config->DefaultPoolSizePerClass = 1;
			Config->MaxPoolSizePerClass = 1;

			Backend = NewObject<UCrowdyActorPoolBackend>(World);
			Backend->InitializeForTest(Pool, Entities, Config);
		}

		void Activate(const int32 SlotId, const FGuid& UUID) const
		{
			Backend->ActivateInstance(SlotId, UUID, ACrowdyStateHostOverrideActor::StaticClass(), FInstancedStruct());
		}

		// What FinishRemoteSpawn leaves behind: an actor of the entity's class registered as its remote proxy, which
		// has already been handed the spawn data.
		AActor* SpawnFromEvent(const FGuid& UUID, const FInstancedStruct& InitialState) const
		{
			ACrowdyStateHostOverrideActor* Actor = World->SpawnActor<ACrowdyStateHostOverrideActor>();

			FCrowdyEntityRecord Record;
			Record.NetID = UUID;
			Record.Role = ECrowdyRole::RemoteProxy;
			Record.Participant = Actor;
			Entities->RegisterEntity(Record);

			Listen(Actor);
			Actor->Entity->DeliverSpawnData(InitialState, false);
			return Actor;
		}

		void Listen(const AActor* Actor) const
		{
			UCrowdyEntityComponent* Component = Actor ? Actor->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
			if (Component)
				Component->OnCrowdySpawned.AddDynamic(Listener.Get(), &UCrowdySpawnDataListener::OnSpawned);
		}

		void ReceiveSpawnEvent(const FGuid& UUID, const FInstancedStruct& InitialState) const
		{
			FCrowdyEntitySpawnEvent Event;
			Event.EntityID = UUID;
			Event.WriterID = FGuid::NewGuid();
			Event.InitialState = InitialState;
			Entities->HandleRemoteSpawnForTest(Event);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolAdoptsSpawnEventActorTest,
	"CrowdySDK.CrowdyReplication.PoolBackendAdoptsSpawnEventActor", CrowdyAppearanceTestFlags)
bool FCrowdyPoolAdoptsSpawnEventActorTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	const FCrowdyAppearanceFixture Fixture(TestWorld.World);

	const FGuid Entity = FGuid::NewGuid();
	AActor* Spawned = Fixture.SpawnFromEvent(Entity, MakeAppearanceState(7.0));

	Fixture.Activate(0, Entity);
	TestTrue(TEXT("the slot draws the actor the spawn event made"), Fixture.Backend->GetSlotActorForTest(0) == Spawned);
	TestTrue(TEXT("which is still alive"), IsValid(Spawned));
	TestTrue(TEXT("and still named by the entity's record"), Fixture.Entities->FindEntity(Entity) == Spawned);
	TestTrue(TEXT("the slot is active"), Fixture.Backend->IsInstanceActive(0));

	TestEqual(TEXT("the drawn actor received the spawn data once"), Fixture.Listener->Calls, 1);
	TestEqual(TEXT("with the spawn event's state"), AppearanceStateX(Fixture.Listener->LastState), 7.0);

	// A repeated spawn event finds the actor already drawn and does not hand it the data a second time.
	Fixture.ReceiveSpawnEvent(Entity, MakeAppearanceState(9.0));
	TestEqual(TEXT("a repeated spawn event does not deliver again"), Fixture.Listener->Calls, 1);

	// The pool's only actor was never taken, so the next entity gets it without hitting the cap.
	const FGuid Other = FGuid::NewGuid();
	Fixture.Activate(1, Other);
	AActor* PoolActor = Fixture.Backend->GetSlotActorForTest(1);
	TestTrue(TEXT("the pool actor is still free for the next entity"), IsValid(PoolActor) && PoolActor != Spawned);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolProxyOfLeaderEntityTest,
	"CrowdySDK.CrowdyReplication.PoolProxyOfLeaderEntityNamesTheLeader", CrowdyAppearanceTestFlags)
bool FCrowdyPoolProxyOfLeaderEntityTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	const FCrowdyAppearanceFixture Fixture(TestWorld.World);

	const FGuid Leader = FGuid::NewGuid();
	Fixture.Entities->SetLocalPlayerID(FGuid::NewGuid());
	Fixture.Entities->SetGameSessionForTest(MakeGameSession(Leader));

	const FGuid Entity = FGuid::NewGuid();
	Fixture.Backend->ActivateInstance(0, Entity, ACrowdyLeaderOwnedTestActor::StaticClass(), FInstancedStruct());

	const FCrowdyEntityRecord* Record = Fixture.Entities->FindRecord(Entity);
	if (!TestNotNull(TEXT("the pool registered the proxy"), Record))
	{
		return false;
	}
	TestTrue(TEXT("as a Leader entity"), Record->bLeaderOwned);
	TestEqual(TEXT("owned by the Leader"), Record->WriterID, Leader);

	const UCrowdyEntityComponent* Component = Record->GetActor()->FindComponentByClass<UCrowdyEntityComponent>();
	TestEqual(TEXT("and its component agrees"), Component->GetWriterID(), Leader);
	return true;
}

// Another player's avatar sends no spawn event, so the pool names it from its class: a class whose entity component
// takes the Player Derived policy draws a player's avatar, asked once per class. One a spawn event made is not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPooledAvatarIsFlaggedFromItsClassTest,
	"CrowdySDK.ActorManager.PooledAvatarIsFlaggedFromItsClass", CrowdyAppearanceTestFlags)
bool FCrowdyPooledAvatarIsFlaggedFromItsClassTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	const FCrowdyAppearanceFixture Fixture(TestWorld.World);
	Fixture.Config->DefaultPoolSizePerClass = 3;
	Fixture.Config->MaxPoolSizePerClass = 3;

	const auto Draw = [&Fixture](const int32 SlotId, UClass* Class) -> const FCrowdyEntityRecord*
	{
		const FGuid Entity = FGuid::NewGuid();
		Fixture.Backend->ActivateInstance(SlotId, Entity, Class, FInstancedStruct());
		return Fixture.Entities->FindRecord(Entity);
	};

	const FCrowdyEntityRecord* First = Draw(0, ACrowdyPlayerDerivedTestPawn::StaticClass());
	if (!TestNotNull(TEXT("the pool drew a Player Derived pawn"), First))
	{
		return false;
	}
	TestTrue(TEXT("as a player's avatar"), First->bAvatar);
	TestFalse(TEXT("whose writer is not known yet"), First->WriterID.IsValid());

	const FCrowdyEntityRecord* Second = Draw(1, ACrowdyPlayerDerivedTestPawn::StaticClass());
	TestTrue(TEXT("a second pawn of the class is one too"), Second && Second->bAvatar);
	TestEqual(TEXT("with the class asked once"), Fixture.Backend->AvatarClassResolutionCount, 1);

	const FCrowdyEntityRecord* Prop = Draw(2, ACrowdyStateHostOverrideActor::StaticClass());
	TestTrue(TEXT("an entity of another policy is not"), Prop && !Prop->bAvatar);
	TestEqual(TEXT("its class asked once as well"), Fixture.Backend->AvatarClassResolutionCount, 2);

	// What FinishRemoteSpawn leaves for a spawn event: the slot adopts that actor and its record.
	const FGuid Spawned = FGuid::NewGuid();
	ACrowdyPlayerDerivedTestPawn* SpawnedPawn = TestWorld.World->SpawnActor<ACrowdyPlayerDerivedTestPawn>();
	FCrowdyEntityRecord SpawnRecord;
	SpawnRecord.NetID = Spawned;
	SpawnRecord.WriterID = FGuid::NewGuid();
	SpawnRecord.Role = ECrowdyRole::RemoteProxy;
	SpawnRecord.Participant = SpawnedPawn;
	Fixture.Entities->RegisterEntity(SpawnRecord);
	Fixture.Backend->ActivateInstance(3, Spawned, ACrowdyPlayerDerivedTestPawn::StaticClass(), FInstancedStruct());
	const FCrowdyEntityRecord* Adopted = Fixture.Entities->FindRecord(Spawned);
	TestTrue(TEXT("a pawn a spawn event made is drawn as that actor"), Adopted && Adopted->GetActor() == SpawnedPawn);
	TestTrue(TEXT("and is not a player's avatar"), Adopted && !Adopted->bAvatar);

	// A spawn event whose actor is not there yet: the pool draws its own actor in its place, still not an avatar, and
	// keeps what the spawn event said about it.
	constexpr int64 PendingUser = 4242;
	constexpr uint32 PendingClass = 77;
	const FGuid Pending = FGuid::NewGuid();
	FCrowdyEntityRecord PendingRecord;
	PendingRecord.NetID = Pending;
	PendingRecord.WriterID = FGuid::NewGuid();
	PendingRecord.OwnerUserID = PendingUser;
	PendingRecord.ClassID = PendingClass;
	PendingRecord.Role = ECrowdyRole::RemoteProxy;
	Fixture.Entities->RegisterEntity(PendingRecord);
	Fixture.Backend->ActivateInstance(4, Pending, ACrowdyPlayerDerivedTestPawn::StaticClass(), FInstancedStruct());
	const FCrowdyEntityRecord* Replaced = Fixture.Entities->FindRecord(Pending);
	const AActor* PoolActor = Fixture.Backend->GetSlotActorForTest(4);
	if (!TestTrue(TEXT("a spawn event's entity drawn by a pool actor"), IsValid(PoolActor) && Replaced && Replaced->GetActor() == PoolActor))
	{
		return false;
	}
	TestFalse(TEXT("is not a player's avatar either"), Replaced->bAvatar);
	TestEqual(TEXT("with the spawn event's writer"), Replaced->WriterID, PendingRecord.WriterID);
	TestEqual(TEXT("its owner user"), Replaced->OwnerUserID, PendingUser);
	TestEqual(TEXT("and its class"), Replaced->ClassID, PendingClass);
	const UCrowdyEntityComponent* PoolComponent = PoolActor->FindComponentByClass<UCrowdyEntityComponent>();
	TestTrue(TEXT("which the pool actor's component names too"), PoolComponent && PoolComponent->GetWriterID() == PendingRecord.WriterID
		&& PoolComponent->GetOwnerUserID() == PendingUser);
	return true;
}

// A pawn of a Player Derived class that is not a player's avatar sends under its writer's tagged id, so it is never
// drawn as an avatar, whichever of its spawn event and its first update arrives first. A real avatar sends under its
// bare id and announces no spawn, so a spawn event naming one changes nothing; only a forged spawn that beats the
// avatar's first update gets the entity drawn as it describes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAvatarFlagFollowsHeardWireIdTest,
	"CrowdySDK.ActorManager.AvatarFlagFollowsTheHeardWireId", CrowdyAppearanceTestFlags)
bool FCrowdyAvatarFlagFollowsHeardWireIdTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	const FCrowdyAppearanceFixture Fixture(TestWorld.World);
	Fixture.Config->DefaultPoolSizePerClass = 3;
	Fixture.Config->MaxPoolSizePerClass = 3;
	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	Fixture.Entities->ActorTrackerForTest = Tracker.Get();
	UClass* PawnClass = ACrowdyPlayerDerivedTestPawn::StaticClass();

	const auto Hear = [&Tracker](const FGuid& NetID, const FCrowdyActorId& WireId)
	{
		const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message = MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
		Message->UUID = WireId;
		FCrowdyActorUpdate Update;
		Update.UUID = NetID;
		Update.Message = Message;
		Tracker->RecordAppearancesForTest({ Update });
	};
	const auto Spawn = [&Fixture](const FGuid& NetID, const FGuid& WriterID, const int64 OwnerUserID, const double X)
	{
		FCrowdyEntitySpawnEvent Event;
		Event.EntityID = NetID;
		Event.WriterID = WriterID;
		Event.OwnerUserID = OwnerUserID;
		Event.InitialState = MakeAppearanceState(X);
		Fixture.Entities->HandleRemoteSpawnForTest(Event);
	};
	const auto SpawnedFirst = [&Fixture, &TestWorld](const FGuid& NetID, const FGuid& WriterID, const int64 OwnerUserID)
	{
		ACrowdyPlayerDerivedTestPawn* Pawn = TestWorld.World->SpawnActor<ACrowdyPlayerDerivedTestPawn>();
		FCrowdyEntityRecord Record;
		Record.NetID = NetID;
		Record.WriterID = WriterID;
		Record.OwnerUserID = OwnerUserID;
		Record.Role = ECrowdyRole::RemoteProxy;
		Record.Participant = Pawn;
		Fixture.Entities->RegisterEntity(Record);
		return Pawn;
	};

	// A spawned pawn whose first update arrives before its spawn event.
	constexpr int64 SpawnerUser = 5001;
	const FGuid Spawner = FGuid::NewGuid();
	const FGuid Early = FGuid::NewGuid();
	const FCrowdyActorId EarlyWireId = FCrowdyActorId::ForOwner(Early, Spawner);
	Hear(Early, EarlyWireId);
	Fixture.Backend->ActivateInstance(0, Early, PawnClass, FInstancedStruct());
	const FCrowdyEntityRecord* EarlyRecord = Fixture.Entities->FindRecord(Early);
	if (!TestNotNull(TEXT("the pool drew the spawned pawn"), EarlyRecord))
	{
		return false;
	}
	TestFalse(TEXT("heard under its writer's id, it is not a player's avatar"), EarlyRecord->bAvatar);
	TestTrue(TEXT("so a targeted message goes to that id"), Fixture.Entities->ResolveTargetWireIdForTest(Early, Tracker.Get()) == EarlyWireId);
	Fixture.Listen(EarlyRecord->GetActor());

	Spawn(Early, Spawner, SpawnerUser, 5.0);
	EarlyRecord = Fixture.Entities->FindRecord(Early);
	TestEqual(TEXT("its late spawn event names the writer"), EarlyRecord->WriterID, Spawner);
	TestEqual(TEXT("and the owner user"), EarlyRecord->OwnerUserID, SpawnerUser);
	TestEqual(TEXT("and hands the pawn its data"), Fixture.Listener->Calls, 1);
	TestTrue(TEXT("a grant for it may re-point it"), CrowdyGrantMayRepoint(Early, EarlyRecord->WriterID, Spawner, false, EarlyRecord->bAvatar));
	TestEqual(TEXT("and a grant to it names its writer"), UCrowdyOwnershipTransfer::ResolveGrantedWriterID(*EarlyRecord), Spawner);

	// The same class with its spawn event first: the slot adopts the actor the spawn made.
	const FGuid Late = FGuid::NewGuid();
	const ACrowdyPlayerDerivedTestPawn* LatePawn = SpawnedFirst(Late, Spawner, SpawnerUser);
	Hear(Late, FCrowdyActorId::ForOwner(Late, Spawner));
	Fixture.Backend->ActivateInstance(1, Late, PawnClass, FInstancedStruct());
	const FCrowdyEntityRecord* LateRecord = Fixture.Entities->FindRecord(Late);
	TestTrue(TEXT("a pawn whose spawn came first is drawn as that actor"), LateRecord && LateRecord->GetActor() == LatePawn);
	TestTrue(TEXT("and is not a player's avatar either"), LateRecord && !LateRecord->bAvatar && LateRecord->WriterID == Spawner);

	// A real avatar, then a spawn event naming it.
	constexpr int64 ForgerUser = 6002;
	const FGuid Forger = FGuid::NewGuid();
	const FGuid Avatar = FGuid::NewGuid();
	Hear(Avatar, FCrowdyActorId::FromGuid(Avatar));
	Fixture.Backend->ActivateInstance(2, Avatar, PawnClass, FInstancedStruct());
	const FCrowdyEntityRecord* AvatarRecord = Fixture.Entities->FindRecord(Avatar);
	if (!TestNotNull(TEXT("the pool drew the avatar"), AvatarRecord))
	{
		return false;
	}
	TestTrue(TEXT("heard under its bare id, it is a player's avatar"), AvatarRecord->bAvatar);
	Fixture.Listen(AvatarRecord->GetActor());
	const int32 CallsBeforeForgery = Fixture.Listener->Calls;

	Spawn(Avatar, Forger, ForgerUser, 9.0);
	AvatarRecord = Fixture.Entities->FindRecord(Avatar);
	TestTrue(TEXT("a spawn event naming it leaves it an avatar"), AvatarRecord->bAvatar);
	TestFalse(TEXT("names no writer on it"), AvatarRecord->WriterID.IsValid());
	TestEqual(TEXT("nor a user"), AvatarRecord->OwnerUserID, static_cast<int64>(0));
	TestEqual(TEXT("and delivers no state to it"), Fixture.Listener->Calls, CallsBeforeForgery);
	TestFalse(TEXT("a grant for it is still refused"), CrowdyGrantMayRepoint(Avatar, AvatarRecord->WriterID, Forger, false, AvatarRecord->bAvatar));
	TestEqual(TEXT("a grant to it still names its session"), UCrowdyOwnershipTransfer::ResolveGrantedWriterID(*AvatarRecord), Avatar);
	TestTrue(TEXT("and a targeted message its bare id"), Fixture.Entities->ResolveTargetWireIdForTest(Avatar, Tracker.Get()) == FCrowdyActorId::FromGuid(Avatar));

	// A forged spawn that beats the avatar's first update to this client: drawn as the spawn described.
	const FGuid Squatted = FGuid::NewGuid();
	SpawnedFirst(Squatted, Forger, ForgerUser);
	Hear(Squatted, FCrowdyActorId::FromGuid(Squatted));
	Fixture.Backend->ActivateInstance(3, Squatted, PawnClass, FInstancedStruct());
	const FCrowdyEntityRecord* SquattedRecord = Fixture.Entities->FindRecord(Squatted);
	TestTrue(TEXT("a forged spawn that arrived first is believed"), SquattedRecord && !SquattedRecord->bAvatar && SquattedRecord->WriterID == Forger);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpawnDataReachesEarlyProxyTest,
	"CrowdySDK.CrowdyReplication.SpawnDataReachesProxyThatArrivedFirst", CrowdyAppearanceTestFlags)
bool FCrowdySpawnDataReachesEarlyProxyTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	const FCrowdyAppearanceFixture Fixture(TestWorld.World);

	// The update came first: the backend drew the entity with a pool actor before any spawn event named it.
	const FGuid Entity = FGuid::NewGuid();
	Fixture.Activate(0, Entity);
	AActor* Proxy = Fixture.Backend->GetSlotActorForTest(0);
	if (!TestTrue(TEXT("the early update is drawn by a pool actor"), IsValid(Proxy) && Fixture.Entities->FindEntity(Entity) == Proxy))
	{
		return false;
	}
	Fixture.Listen(Proxy);

	Fixture.ReceiveSpawnEvent(Entity, MakeAppearanceState(3.0));
	TestEqual(TEXT("the spawn event hands its data to the actor already drawn"), Fixture.Listener->Calls, 1);
	TestEqual(TEXT("with the event's state"), AppearanceStateX(Fixture.Listener->LastState), 3.0);
	TestTrue(TEXT("and spawns no second body"), Fixture.Entities->FindEntity(Entity) == Proxy);

	Fixture.ReceiveSpawnEvent(Entity, MakeAppearanceState(4.0));
	TestEqual(TEXT("a repeated spawn event does not deliver again"), Fixture.Listener->Calls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolSkipsOwnerGhostTest,
	"CrowdySDK.CrowdyReplication.PoolBackendSkipsOwnerCopyByDefault", CrowdyAppearanceTestFlags)
bool FCrowdyPoolSkipsOwnerGhostTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	const FCrowdyAppearanceFixture Fixture(TestWorld.World);

	AActor* OwnedActor = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	const FGuid Owned = FGuid::NewGuid();
	FCrowdyEntityRecord OwnerRecord;
	OwnerRecord.NetID = Owned;
	OwnerRecord.Role = ECrowdyRole::Owner;
	OwnerRecord.Participant = OwnedActor;
	Fixture.Entities->RegisterEntity(OwnerRecord);

	Fixture.Activate(0, Owned);
	TestNull(TEXT("the server's echo of an entity owned here draws nothing"), Fixture.Backend->GetSlotActorForTest(0));
	TestTrue(TEXT("and the slot counts as active, so it is not retried"), Fixture.Backend->IsInstanceActive(0));

	const FGuid Other = FGuid::NewGuid();
	Fixture.Activate(1, Other);
	TestTrue(TEXT("the pool's only actor is still free for a remote entity"), IsValid(Fixture.Backend->GetSlotActorForTest(1)));

	Fixture.Backend->DeactivateInstance(0, Owned);
	TestTrue(TEXT("the owner's own actor survives the slot's release"), IsValid(OwnedActor));
	TestTrue(TEXT("with its record"), Fixture.Entities->FindEntity(Owned) == OwnedActor);
	TestFalse(TEXT("and the released slot is no longer active"), Fixture.Backend->IsInstanceActive(0));
	return true;
}

namespace
{
	/** A tracker, a manager and an entity subsystem wired the way Initialize wires them, over a recording backend. */
	struct FCrowdyLateUpdateFixture
	{
		TStrongObjectPtr<UCrowdyActorTracker> Tracker;
		TStrongObjectPtr<UCrowdyActorManager> Manager;
		TStrongObjectPtr<UCrowdyRecordingBackend> Backend;
		TStrongObjectPtr<UCrowdyEntitySubsystem> Entities;

		FCrowdyLateUpdateFixture()
			: Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()))
			, Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()))
			, Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()))
			, Entities(NewObject<UCrowdyEntitySubsystem>(GetTransientPackage()))
		{
			Manager->SetBackend(Backend.Get());
			Manager->BindToTrackerForTest(Tracker.Get());
			Manager->SetEntitySubsystemForTest(Entities.Get());
			Manager->BindToEntitySubsystemForTest(Entities.Get());
			Manager->RegisterStateClass(FCrowdyActorState::StaticStruct(), AActor::StaticClass());
		}

		void Appear(const FGuid& UUID) const
		{
			Tracker->OnRemoteEntityAppeared.Broadcast(UUID, MakeAppearanceState(1.0), 1);
		}

		void Update(const FGuid& UUID) const
		{
			FCrowdyActorUpdate Update;
			Update.UUID = UUID;
			Update.ServerTimestamp = 1000;
			Update.State = MakeAppearanceState(2.0);

			TArray<FCrowdyActorUpdate> Batch;
			Batch.Add(Update);
			Manager->DrainUpdateBatchForTest(Batch);
		}

		void Destroy(const FGuid& UUID) const
		{
			FCrowdyEntityDestroyEvent Event;
			Event.EntityID = UUID;
			Entities->HandleRemoteDestroyForTest(Event);
		}

		// An entity this client knows, as a pool proxy or a spawn event would have registered it.
		void Know(const FGuid& UUID, const FGuid& WriterID = FGuid())
		{
			UObject* Participant = NewObject<UCrowdySpawnDataListener>(GetTransientPackage());
			Participants.Emplace(Participant);

			FCrowdyEntityRecord Record;
			Record.NetID = UUID;
			Record.WriterID = WriterID;
			Record.Role = ECrowdyRole::RemoteProxy;
			Record.Participant = Participant;
			Entities->RegisterEntity(Record);
		}

		TArray<TStrongObjectPtr<UObject>> Participants;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLateUpdateAfterDestroyTest,
	"CrowdySDK.ActorManager.LateUpdateDoesNotResurrectDestroyedEntity", CrowdyAppearanceTestFlags)
bool FCrowdyLateUpdateAfterDestroyTest::RunTest(const FString& Parameters)
{
	FCrowdyLateUpdateFixture Fixture;

	// A destroy for an id this client never knew is remembered too: a repeat of its spawn may still be on its way.
	const FGuid Unknown = FGuid::NewGuid();
	Fixture.Destroy(Unknown);
	TestTrue(TEXT("an unknown id is remembered"), Fixture.Entities->WasRecentlyDestroyed(Unknown));

	// An update sent just before the destroy reaches this client after it, as the entity's first sighting.
	const FGuid Destroyed = FGuid::NewGuid();
	Fixture.Know(Destroyed);
	Fixture.Destroy(Destroyed);
	TestTrue(TEXT("a destroyed id is remembered"), Fixture.Entities->WasRecentlyDestroyed(Destroyed));
	Fixture.Appear(Destroyed);
	Fixture.Update(Destroyed);
	TestEqual(TEXT("its late updates are not drawn"), Fixture.Backend->ActivatedSlots.Num(), 0);

	const FGuid Control = FGuid::NewGuid();
	Fixture.Appear(Control);
	TestEqual(TEXT("an entity nobody destroyed is drawn"), Fixture.Backend->ActivatedSlots.Num(), 1);

	// Or the destroy took the actor from a drawn slot, and an update still in flight retries it.
	const FGuid Drawn = FGuid::NewGuid();
	const FGuid Quiet = FGuid::NewGuid();
	Fixture.Know(Drawn);
	Fixture.Appear(Drawn);
	Fixture.Appear(Quiet);
	Fixture.Backend->bReportInactive = true;
	Fixture.Destroy(Drawn);
	Fixture.Manager->NotifyEntityUnregisteredForTest(Quiet);

	Fixture.Update(Drawn);
	TestEqual(TEXT("the destroyed entity's late update does not redraw it"), Fixture.Backend->ActivatedSlots.Num(), 3);
	Fixture.Update(Quiet);
	TestEqual(TEXT("while another inactive slot is retried as before"), Fixture.Backend->ActivatedSlots.Num(), 4);

	// Spawn Entity never reuses an id, so a spawn event for one just destroyed is a repeat sent before the destroy. It
	// is dropped before its class is even looked up; this one names none, so a spawn that went ahead would log an error.
	const FGuid Repeated = FGuid::NewGuid();
	Fixture.Know(Repeated);
	Fixture.Destroy(Repeated);
	FCrowdyEntitySpawnEvent Repeat;
	Repeat.EntityID = Repeated;
	Fixture.Entities->HandleRemoteSpawnForTest(Repeat);
	TestNull(TEXT("a spawn event repeated after the destroy builds nothing"), Fixture.Entities->FindRecord(Repeated));

	// An entity that keeps sending after the window is alive, as after a forged destroy: it is drawn again.
	Fixture.Backend->bReportInactive = false;
	FPlatformProcess::Sleep(static_cast<float>(UCrowdyEntitySubsystem::RecentlyDestroyedSeconds) + 0.1f);
	Fixture.Update(Destroyed);
	TestEqual(TEXT("a first sighting held during the window is drawn by an update after it"), Fixture.Backend->ActivatedSlots.Num(), 5);
	Fixture.Update(Drawn);
	TestEqual(TEXT("and so is a slot whose actor the destroy took"), Fixture.Backend->ActivatedSlots.Num(), 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnershipMoveResetsSlotTest,
	"CrowdySDK.ActorManager.OwnershipMoveSetsTheSlotUpAgain", CrowdyAppearanceTestFlags)
bool FCrowdyOwnershipMoveResetsSlotTest::RunTest(const FString& Parameters)
{
	FCrowdyLateUpdateFixture Fixture;
	const FGuid LocalPlayer = FGuid::NewGuid();
	Fixture.Entities->SetLocalPlayerID(LocalPlayer);

	const FGuid Given = FGuid::NewGuid();
	const FGuid Elsewhere = FGuid::NewGuid();
	Fixture.Know(Given, LocalPlayer);
	Fixture.Know(Elsewhere, FGuid::NewGuid());
	Fixture.Appear(Given);
	Fixture.Appear(Elsewhere);
	TestEqual(TEXT("both entities were set up once"), Fixture.Backend->ActivatedSlots.Num(), 2);

	// This client hands its entity away: the slot set up for an owner here is set up again for a remote one.
	Fixture.Entities->ReassignOwnership(Given, FGuid::NewGuid(), FGuid(), 0);
	Fixture.Update(Given);
	TestEqual(TEXT("ownership leaving this client sets the slot up again"), Fixture.Backend->ActivatedSlots.Num(), 3);

	Fixture.Entities->ReassignOwnership(Elsewhere, FGuid::NewGuid(), FGuid(), 0);
	Fixture.Update(Elsewhere);
	TestEqual(TEXT("a hand-over between two other clients leaves the slot alone"), Fixture.Backend->ActivatedSlots.Num(), 3);

	Fixture.Entities->ReassignOwnership(Elsewhere, LocalPlayer, FGuid(), 0);
	Fixture.Update(Elsewhere);
	TestEqual(TEXT("ownership arriving here sets the slot up again"), Fixture.Backend->ActivatedSlots.Num(), 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPoolFollowsOwnershipTest,
	"CrowdySDK.CrowdyReplication.PoolBackendFollowsOwnershipMoves", CrowdyAppearanceTestFlags)
bool FCrowdyPoolFollowsOwnershipTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	const FCrowdyAppearanceFixture Fixture(TestWorld.World);
	const FGuid LocalPlayer = FGuid::NewGuid();
	Fixture.Entities->SetLocalPlayerID(LocalPlayer);

	AActor* OwnedActor = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	const FGuid Owned = FGuid::NewGuid();
	FCrowdyEntityRecord OwnerRecord;
	OwnerRecord.NetID = Owned;
	OwnerRecord.WriterID = LocalPlayer;
	OwnerRecord.Role = ECrowdyRole::Owner;
	OwnerRecord.Participant = OwnedActor;
	Fixture.Entities->RegisterEntity(OwnerRecord);

	Fixture.Activate(0, Owned);
	if (!TestNull(TEXT("the entity owned here is not drawn by the pool"), Fixture.Backend->GetSlotActorForTest(0)))
	{
		return false;
	}

	// Handed to another client: its updates now move this client's actor, which the slot adopts.
	Fixture.Entities->ReassignOwnership(Owned, FGuid::NewGuid(), FGuid(), 0);
	Fixture.Activate(0, Owned);
	TestTrue(TEXT("once handed away, the actor is drawn from the new owner's updates"), Fixture.Backend->GetSlotActorForTest(0) == OwnedActor);
	TestTrue(TEXT("and the slot is active"), Fixture.Backend->IsInstanceActive(0));

	// Handed back: the slot lets go of the actor without destroying it, since this client moves it again.
	Fixture.Entities->ReassignOwnership(Owned, LocalPlayer, FGuid(), 0);
	Fixture.Activate(0, Owned);
	TestNull(TEXT("once owned here again the slot draws nothing"), Fixture.Backend->GetSlotActorForTest(0));
	TestTrue(TEXT("the actor is alive"), IsValid(OwnedActor));
	TestTrue(TEXT("and still registered"), Fixture.Entities->FindEntity(Owned) == OwnedActor);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpawnedStaticEntitySendsPresenceTest,
	"CrowdySDK.CrowdyReplication.SpawnedStaticEntitySendsPresence", CrowdyAppearanceTestFlags)
bool FCrowdySpawnedStaticEntitySendsPresenceTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	UCrowdyAutoReplicator* AutoReplicator = NewObject<UCrowdyAutoReplicator>(TestWorld.World);
	// Outered to the world, because destroying an entity schedules its actor's removal on the world's timers.
	const TStrongObjectPtr<UCrowdyEntitySubsystem> Entities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
	const FGuid LocalPlayer = FGuid::NewGuid();
	Entities->SetLocalPlayerID(LocalPlayer);

	auto MakeEntity = [&](UClass* Class, const ECrowdyRole Role) -> UCrowdyEntityComponent*
	{
		AActor* Actor = TestWorld.World->SpawnActor<AActor>(Class);
		UCrowdyEntityComponent* Component = Actor ? Actor->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
		if (!Component)
			return nullptr;

		FCrowdyEntityRecord Record;
		Record.NetID = FGuid::NewGuid();
		Record.WriterID = Role == ECrowdyRole::Owner ? LocalPlayer : FGuid::NewGuid();
		Record.Role = Role;
		Record.Participant = Actor;
		Entities->RegisterEntity(Record);

		Component->SetAutoReplicatorForTest(AutoReplicator);
		Component->SetCollaboratorsForTest(Entities.Get(), nullptr);
		Component->AssignPooledIdentity(Record.NetID, Record.WriterID, 0, Role, 0);
		return Component;
	};

	UCrowdyEntityComponent* Owned = MakeEntity(ACrowdyStateHostOverrideActor::StaticClass(), ECrowdyRole::Owner);
	UCrowdyEntityComponent* Mirrored = MakeEntity(ACrowdyStateHostOverrideActor::StaticClass(), ECrowdyRole::RemoteProxy);
	UCrowdyEntityComponent* Moving = MakeEntity(ACrowdyStateOverlapActor::StaticClass(), ECrowdyRole::Owner);
	if (!TestNotNull(TEXT("owned"), Owned) || !TestNotNull(TEXT("mirrored"), Mirrored) || !TestNotNull(TEXT("moving"), Moving))
	{
		return false;
	}

	Owned->EnablePresence();
	Mirrored->EnablePresence();
	Moving->EnablePresence();

	bool bPresenceOnly = false;
	TestTrue(TEXT("a spawned Static entity owned here joins the auto replicator"), AutoReplicator->IsRegisteredForTest(Owned, bPresenceOnly));
	TestTrue(TEXT("as a presence entry"), bPresenceOnly);
	TestFalse(TEXT("one mirrored from a peer sends nothing"), AutoReplicator->IsRegisteredForTest(Mirrored, bPresenceOnly));
	TestFalse(TEXT("a Dynamic entity is left to its own channel"), AutoReplicator->IsRegisteredForTest(Moving, bPresenceOnly));

	// Ownership moving here starts the presence sends, and moving away stops them.
	Entities->ReassignOwnership(Mirrored->GetNetID(), LocalPlayer, FGuid(), 0);
	TestTrue(TEXT("a mirrored entity handed to this client starts sending presence"), AutoReplicator->IsRegisteredForTest(Mirrored, bPresenceOnly) && bPresenceOnly);
	Entities->ReassignOwnership(Owned->GetNetID(), FGuid::NewGuid(), FGuid(), 0);
	TestFalse(TEXT("one handed away stops"), AutoReplicator->IsRegisteredForTest(Owned, bPresenceOnly));

	// A presence send is the spawn event again, from where the entity is now, with its owner and its spawn data.
	Mirrored->DeliverSpawnData(MakeAppearanceState(7.0), false);
	Mirrored->GetOwner()->SetActorLocation(FVector(300.0, 40.0, 5.0));
	Entities->bCaptureAnnouncedSpawnsForTest = true;
	Mirrored->SendPresence();
	if (TestEqual(TEXT("a presence send announces the spawn once"), Entities->AnnouncedSpawnsForTest.Num(), 1))
	{
		const FCrowdyEntitySpawnEvent& Sent = Entities->AnnouncedSpawnsForTest[0];
		TestEqual(TEXT("for the entity"), Sent.EntityID, Mirrored->GetNetID());
		TestEqual(TEXT("naming this client, its owner now"), Sent.WriterID, LocalPlayer);
		TestTrue(TEXT("from where the entity is now"), Sent.SpawnTransform.GetLocation().Equals(FVector(300.0, 40.0, 5.0)));
		TestEqual(TEXT("with the spawn data it was given"), AppearanceStateX(Sent.InitialState), 7.0);
		TestEqual(TEXT("and its class, so a client that never loaded it can"), Sent.ClassPath, ACrowdyStateHostOverrideActor::StaticClass()->GetPathName());

		// A client that joined after the first spawn builds the entity from this one, owner and position included.
		const TStrongObjectPtr<UCrowdyEntitySubsystem> LateJoiner(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
		LateJoiner->HandleRemoteSpawnForTest(Sent);
		const UCrowdyEntityComponent* Built = nullptr;
		for (TActorIterator<ACrowdyStateHostOverrideActor> It(TestWorld.World); It; ++It)
		{
			if (It->Entity && It->Entity->GetNetID() == Sent.EntityID && It->Entity != Mirrored)
				Built = It->Entity;
		}
		if (TestNotNull(TEXT("a client that joined later builds the entity from it"), Built))
		{
			TestEqual(TEXT("as a remote copy"), Built->GetRole(), ECrowdyRole::RemoteProxy);
			TestEqual(TEXT("that knows its owner"), Built->GetWriterID(), LocalPlayer);
			TestTrue(TEXT("where the entity is now"), Built->GetOwner()->GetActorLocation().Equals(FVector(300.0, 40.0, 5.0)));
		}
	}

	// A repeat names the owner the entity has, not whoever sends it: the first entity was handed to another player.
	Entities->AnnouncedSpawnsForTest.Reset();
	Owned->SendPresence();
	if (TestEqual(TEXT("a send for the handed-away entity announces once"), Entities->AnnouncedSpawnsForTest.Num(), 1))
	{
		TestEqual(TEXT("naming the entity's own owner, not this client"), Entities->AnnouncedSpawnsForTest[0].WriterID, Owned->GetWriterID());
		TestNotEqual(TEXT("which is another player"), Entities->AnnouncedSpawnsForTest[0].WriterID, LocalPlayer);
	}

	// Destroying it stops the sends at once, not when its Destroy Delay runs out and the actor goes.
	Mirrored->DestroyDelay = 5.f;
	Entities->DestroyEntity(Mirrored->GetOwner());
	TestFalse(TEXT("a destroyed entity stops sending at once"), AutoReplicator->IsRegisteredForTest(Mirrored, bPresenceOnly));
	TestTrue(TEXT("and the destroying client remembers it, so a repeat already on its way cannot rebuild it"),
		Entities->WasRecentlyDestroyed(Mirrored->GetNetID()));
	return true;
}

// A spawn event names its writer's actor id and its owner's user: the signed-in user, read when it is sent, for an
// entity this client writes; the user its writer claimed for one it repeats for a peer; none for a Leader entity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpawnEventCarriesWriterAndOwnerUserTest,
	"CrowdySDK.Entity.SpawnEventCarriesWriterAndOwnerUser", CrowdyAppearanceTestFlags)
bool FCrowdySpawnEventCarriesWriterAndOwnerUserTest::RunTest(const FString& Parameters)
{
	constexpr int64 SignedInUser = 8080;
	constexpr int64 NextUser = 8081;
	constexpr int64 PeerUser = 777;
	FCrowdyAppearanceTestWorld TestWorld;
	UCrowdyAutoReplicator* AutoReplicator = NewObject<UCrowdyAutoReplicator>(TestWorld.World);
	const TStrongObjectPtr<UCrowdyEntitySubsystem> Entities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
	const FGuid LocalPlayer = FGuid::NewGuid();
	Entities->SetLocalPlayerID(LocalPlayer);
	UCrowdyGameSession* Session = MakeGameSession(FGuid());
	Session->SetUserID(SignedInUser);
	Entities->SetGameSessionForTest(Session);
	Entities->bCaptureAnnouncedSpawnsForTest = true;

	auto MakeEntity = [&](const FGuid& Writer, const ECrowdyRole Role, const int64 ClaimedUser, const bool bLeaderOwned) -> UCrowdyEntityComponent*
	{
		AActor* Actor = TestWorld.World->SpawnActor<AActor>(ACrowdyStateHostOverrideActor::StaticClass());
		UCrowdyEntityComponent* Component = Actor ? Actor->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
		if (!Component)
			return nullptr;

		FCrowdyEntityRecord Record;
		Record.NetID = FGuid::NewGuid();
		Record.WriterID = Writer;
		Record.OwnerUserID = ClaimedUser;
		Record.Role = Role;
		Record.bLeaderOwned = bLeaderOwned;
		Record.Participant = Actor;
		Entities->RegisterEntity(Record);

		Component->SetAutoReplicatorForTest(AutoReplicator);
		Component->SetCollaboratorsForTest(Entities.Get(), nullptr);
		Component->AssignPooledIdentity(Record.NetID, Writer, ClaimedUser, Role, 0);
		return Component;
	};

	auto Announce = [&Entities](const UCrowdyEntityComponent* Component) -> const FCrowdyEntitySpawnEvent*
	{
		Entities->AnnouncedSpawnsForTest.Reset();
		Component->SendPresence();
		return Entities->AnnouncedSpawnsForTest.Num() == 1 ? &Entities->AnnouncedSpawnsForTest[0] : nullptr;
	};

	const FGuid Peer = FGuid::NewGuid();
	const UCrowdyEntityComponent* Mine = MakeEntity(LocalPlayer, ECrowdyRole::Owner, 0, false);
	const UCrowdyEntityComponent* Theirs = MakeEntity(Peer, ECrowdyRole::RemoteProxy, PeerUser, false);
	const UCrowdyEntityComponent* Leaders = MakeEntity(LocalPlayer, ECrowdyRole::Owner, 0, true);
	if (!TestNotNull(TEXT("mine"), Mine) || !TestNotNull(TEXT("theirs"), Theirs) || !TestNotNull(TEXT("the Leader's"), Leaders))
	{
		return false;
	}

	const FCrowdyEntitySpawnEvent* Sent = Announce(Mine);
	if (!TestNotNull(TEXT("an entity this client writes announces its spawn"), Sent))
	{
		return false;
	}
	TestEqual(TEXT("naming this session as its writer"), Sent->WriterID, LocalPlayer);
	TestEqual(TEXT("and the signed-in user as its owner"), Sent->OwnerUserID, SignedInUser);

	Session->SetUserID(NextUser);
	Sent = Announce(Mine);
	TestTrue(TEXT("read when it is sent, so another sign-in is named"), Sent && Sent->OwnerUserID == NextUser);

	Sent = Announce(Theirs);
	TestTrue(TEXT("a repeat for a peer's entity names that peer"), Sent && Sent->WriterID == Peer);
	TestTrue(TEXT("and the user it claimed"), Sent && Sent->OwnerUserID == PeerUser);

	Sent = Announce(Leaders);
	TestTrue(TEXT("a Leader entity names no user, though this client writes it"), Sent && Sent->OwnerUserID == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpawnEventTeachesEarlyProxyItsOwnerTest,
	"CrowdySDK.CrowdyReplication.SpawnEventTeachesAnEarlyProxyItsOwner", CrowdyAppearanceTestFlags)
bool FCrowdySpawnEventTeachesEarlyProxyItsOwnerTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	const FCrowdyAppearanceFixture Fixture(TestWorld.World);

	// The update came first, so the pool drew the entity before anything named its owner.
	const FGuid Entity = FGuid::NewGuid();
	Fixture.Activate(0, Entity);
	const AActor* Proxy = Fixture.Backend->GetSlotActorForTest(0);
	const UCrowdyEntityComponent* Component = Proxy ? Proxy->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
	if (!TestNotNull(TEXT("the early update is drawn by a pool actor with an entity component"), Component))
	{
		return false;
	}
	TestFalse(TEXT("which does not know its owner yet"), Component->GetWriterID().IsValid());

	FCrowdyEntitySpawnEvent Event;
	Event.EntityID = Entity;
	Event.WriterID = FGuid::NewGuid();
	Event.InitialState = MakeAppearanceState(3.0);
	Fixture.Entities->HandleRemoteSpawnForTest(Event);

	TestEqual(TEXT("the spawn event names the owner on the record"), Fixture.Entities->FindRecord(Entity)->WriterID, Event.WriterID);
	TestEqual(TEXT("and on the actor already drawn for it"), Component->GetWriterID(), Event.WriterID);
	TestEqual(TEXT("which keeps its id and role"), Component->GetNetID(), Entity);
	TestEqual(TEXT("as a remote copy"), Component->GetRole(), ECrowdyRole::RemoteProxy);
	return true;
}

// A spawn event teaches a proxy its owner's user once, from the writer the record already names. A known user is never
// replaced by a later spawn, and neither a Leader entity nor one naming this client as its writer takes a peer's claim.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySpawnEventTeachesProxyItsOwnerUserTest,
	"CrowdySDK.CrowdyReplication.SpawnEventTeachesAProxyItsOwnerUser", CrowdyAppearanceTestFlags)
bool FCrowdySpawnEventTeachesProxyItsOwnerUserTest::RunTest(const FString& Parameters)
{
	constexpr int64 FirstUser = 111;
	constexpr int64 LaterUser = 222;
	constexpr int64 Unknown = 0;
	FCrowdyAppearanceTestWorld TestWorld;
	const FCrowdyAppearanceFixture Fixture(TestWorld.World);
	const FGuid Local = FGuid::NewGuid();
	Fixture.Entities->SetLocalPlayerID(Local);

	// The update came first, so the pool drew the entity before anything named its owner.
	const FGuid Entity = FGuid::NewGuid();
	Fixture.Activate(0, Entity);
	const AActor* Proxy = Fixture.Backend->GetSlotActorForTest(0);
	const UCrowdyEntityComponent* Component = Proxy ? Proxy->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
	if (!TestNotNull(TEXT("the early update is drawn by a pool actor with an entity component"), Component))
	{
		return false;
	}
	TestEqual(TEXT("which does not know its owner's user yet"), Component->GetOwnerUserID(), Unknown);

	FCrowdyEntitySpawnEvent Event;
	Event.EntityID = Entity;
	Event.WriterID = FGuid::NewGuid();
	Event.OwnerUserID = FirstUser;
	Fixture.Entities->HandleRemoteSpawnForTest(Event);
	TestEqual(TEXT("the spawn event names the owner's user on the record"), Fixture.Entities->FindRecord(Entity)->OwnerUserID, FirstUser);
	TestEqual(TEXT("which the actor drawn for it reads"), Component->GetOwnerUserID(), FirstUser);

	Event.OwnerUserID = LaterUser;
	Fixture.Entities->HandleRemoteSpawnForTest(Event);
	TestEqual(TEXT("a later spawn event never replaces a known user"), Fixture.Entities->FindRecord(Entity)->OwnerUserID, FirstUser);

	// Each case has no body behind its record, so only the backfill runs.
	auto Arrive = [&Fixture](const FGuid& Writer, const bool bLeaderOwned, const FGuid& EventWriter) -> int64
	{
		FCrowdyEntityRecord Record;
		Record.NetID = FGuid::NewGuid();
		Record.WriterID = Writer;
		Record.Role = ECrowdyRole::RemoteProxy;
		Record.bLeaderOwned = bLeaderOwned;
		Fixture.Entities->RegisterEntity(Record);

		FCrowdyEntitySpawnEvent Claim;
		Claim.EntityID = Record.NetID;
		Claim.WriterID = EventWriter;
		Claim.OwnerUserID = LaterUser;
		Fixture.Entities->HandleRemoteSpawnForTest(Claim);
		return Fixture.Entities->FindRecord(Record.NetID)->OwnerUserID;
	};

	const FGuid Peer = FGuid::NewGuid();
	TestEqual(TEXT("a spawn from a writer the record does not name teaches nothing"), Arrive(Peer, false, FGuid::NewGuid()), Unknown);
	TestEqual(TEXT("nor does one naming this client as the writer"), Arrive(Local, false, Local), Unknown);
	TestEqual(TEXT("and a Leader entity keeps no user"), Arrive(Peer, true, Peer), Unknown);
	TestEqual(TEXT("while the writer it names teaches it"), Arrive(Peer, false, Peer), LaterUser);
	return true;
}

namespace
{
	/** A world whose actor manager draws through Backend, bound to its entity subsystem as Initialize binds them. */
	struct FCrowdyCrowdRowFixture
	{
		FCrowdyAppearanceTestWorld TestWorld;
		TStrongObjectPtr<UCrowdyEntitySubsystem> Entities;
		TStrongObjectPtr<UCrowdyActorTracker> Tracker;
		TStrongObjectPtr<UCrowdyActorManager> Manager;

		explicit FCrowdyCrowdRowFixture(UCrowdyRecordingBackend* Backend)
			// Outered to the world, because a spawn event that does spawn an actor spawns it there.
			: Entities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World))
			, Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()))
			, Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()))
		{
			Manager->SetBackend(Backend);
			Manager->BindToTrackerForTest(Tracker.Get());
			Manager->SetEntitySubsystemForTest(Entities.Get());
			Manager->BindToEntitySubsystemForTest(Entities.Get());
			Manager->RegisterStateClass(FCrowdyActorState::StaticStruct(), AActor::StaticClass());
		}

		FGuid ReceiveSpawn(UClass* Class, const double X) const
		{
			FCrowdyEntitySpawnEvent Event;
			Event.EntityID = FGuid::NewGuid();
			Event.WriterID = FGuid::NewGuid();
			Event.ClassID = UCrowdyClassRegistry::Get()->GetID(Class);
			Event.ClassPath = Class->GetPathName();
			Event.InitialState = MakeAppearanceState(X);
			Entities->HandleRemoteSpawnForTest(Event);
			return Event.EntityID;
		}

		bool HasActorFor(const FGuid& NetID) const
		{
			for (TActorIterator<AActor> It(TestWorld.World); It; ++It)
			{
				const UCrowdyEntityComponent* Component = It->FindComponentByClass<UCrowdyEntityComponent>();
				if (Component && Component->GetNetID() == NetID)
					return true;
			}
			return false;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdRowMapSpawnsNoActorTest,
	"CrowdySDK.CrowdyReplication.CrowdRowMapSpawnsNoActorForAMovingEntity", CrowdyAppearanceTestFlags)
bool FCrowdyCrowdRowMapSpawnsNoActorTest::RunTest(const FString& Parameters)
{
	UCrowdyCrowdRowRecordingBackend* Rows = NewObject<UCrowdyCrowdRowRecordingBackend>(GetTransientPackage());
	const FCrowdyCrowdRowFixture Fixture(Rows);

	// A moving entity on a map drawn as crowd rows: the row is its only body, and its spawn data goes to the backend.
	const FGuid Moving = Fixture.ReceiveSpawn(ACrowdyStateOverlapActor::StaticClass(), 5.0);
	TestFalse(TEXT("a moving entity's spawn event spawns no actor on a crowd-row map"), Fixture.HasActorFor(Moving));
	if (TestEqual(TEXT("its spawn data is handed to the backend once"), Rows->SpawnDataIDs.Num(), 1))
	{
		TestEqual(TEXT("for that entity"), Rows->SpawnDataIDs[0], Moving);
		TestEqual(TEXT("with the event's state"), AppearanceStateX(Rows->SpawnData[0]), 5.0);
	}

	// A Static entity sends no updates, so no row is ever drawn for it: it keeps its actor.
	const FGuid Still = Fixture.ReceiveSpawn(ACrowdyStateHostOverrideActor::StaticClass(), 6.0);
	TestTrue(TEXT("a Static entity still gets its actor"), Fixture.HasActorFor(Still));
	TestEqual(TEXT("and its spawn data is not handed to the backend"), Rows->SpawnDataIDs.Num(), 1);

	// The update came first, while the class was unknown, so it waits for the spawn event: no actor will register to
	// resolve it, so the spawn event activates it.
	const FGuid Parked = FGuid::NewGuid();
	Fixture.Tracker->OnRemoteEntityAppeared.Broadcast(Parked, FInstancedStruct::Make(FCrowdyEntityDestroyEvent()), 1);
	TestEqual(TEXT("an update whose class is unknown is not drawn yet"), Rows->ActivatedSlots.Num(), 0);
	FCrowdyEntitySpawnEvent Late;
	Late.EntityID = Parked;
	Late.ClassID = UCrowdyClassRegistry::Get()->GetID(ACrowdyStateOverlapActor::StaticClass());
	Late.ClassPath = ACrowdyStateOverlapActor::StaticClass()->GetPathName();
	Fixture.Entities->HandleRemoteSpawnForTest(Late);
	TestEqual(TEXT("its spawn event draws it as a row"), Rows->ActivatedSlots.Num(), 1);
	TestTrue(TEXT("of the class the spawn event named"), Rows->ActivatedClasses.Num() == 1 && Rows->ActivatedClasses[0] == ACrowdyStateOverlapActor::StaticClass());
	TestFalse(TEXT("with no actor"), Fixture.HasActorFor(Parked));

	// On a map drawn with actors the moving entity gets its actor, as before.
	Fixture.Manager->SetBackend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));
	const FGuid Pooled = Fixture.ReceiveSpawn(ACrowdyStateOverlapActor::StaticClass(), 7.0);
	TestTrue(TEXT("a moving entity on an actor map still gets its actor"), Fixture.HasActorFor(Pooled));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderClassOnRowsWarnsOnceTest,
	"CrowdySDK.LeaderOwned.ACrowdRowMapSaysALeaderClassCannotBeTakenOver", CrowdyAppearanceTestFlags)
bool FCrowdyLeaderClassOnRowsWarnsOnceTest::RunTest(const FString& Parameters)
{
	UCrowdyCrowdRowRecordingBackend* Rows = NewObject<UCrowdyCrowdRowRecordingBackend>(GetTransientPackage());
	const FCrowdyCrowdRowFixture Fixture(Rows);

	Fixture.ReceiveSpawn(ACrowdyStateOverlapActor::StaticClass(), 1.0);
	TestEqual(TEXT("a Player class says nothing"), Fixture.Entities->GetReportedLeaderRowClassCountForTest(), 0);

	Fixture.ReceiveSpawn(ACrowdyLeaderMovingTestActor::StaticClass(), 2.0);
	Fixture.ReceiveSpawn(ACrowdyLeaderMovingTestActor::StaticClass(), 3.0);
	TestEqual(TEXT("a Leader class is reported once"), Fixture.Entities->GetReportedLeaderRowClassCountForTest(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeadingClientRefusesPoolBodyTest,
	"CrowdySDK.LeaderOwned.AnUpdateGivesTheLeaderNoBodyItNeverHad", CrowdyAppearanceTestFlags)
bool FCrowdyLeadingClientRefusesPoolBodyTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	const FCrowdyAppearanceFixture Fixture(TestWorld.World);
	const FGuid Local = FGuid::NewGuid();
	Fixture.Entities->SetLocalPlayerID(Local);
	Fixture.Entities->SetGameSessionForTest(MakeGameSession(Local));

	const FGuid Entity = FGuid::NewGuid();
	Fixture.Backend->ActivateInstance(0, Entity, ACrowdyLeaderMovingTestActor::StaticClass(), FInstancedStruct());
	TestNull(TEXT("the client that leads takes no pool actor for it"), Fixture.Entities->FindEntity(Entity));
	TestFalse(TEXT("and the slot stays open"), Fixture.Backend->IsInstanceActive(0));

	Fixture.Entities->SetGameSessionForTest(MakeGameSession(FGuid::NewGuid()));
	Fixture.Backend->ActivateInstance(0, Entity, ACrowdyLeaderMovingTestActor::StaticClass(), FInstancedStruct());
	TestNotNull(TEXT("a client that does not lead draws it"), Fixture.Entities->FindEntity(Entity));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDestroyEventReleasesCrowdRowTest,
	"CrowdySDK.ActorManager.DestroyEventReleasesACrowdRow", CrowdyAppearanceTestFlags)
bool FCrowdyDestroyEventReleasesCrowdRowTest::RunTest(const FString& Parameters)
{
	UCrowdyCrowdRowRecordingBackend* Rows = NewObject<UCrowdyCrowdRowRecordingBackend>(GetTransientPackage());
	const FCrowdyCrowdRowFixture Fixture(Rows);

	auto Update = [&Fixture](const FGuid& UUID)
	{
		FCrowdyActorUpdate Update;
		Update.UUID = UUID;
		Update.ServerTimestamp = 1000;
		Update.State = MakeAppearanceState(2.0);
		TArray<FCrowdyActorUpdate> Batch;
		Batch.Add(Update);
		Fixture.Manager->DrainUpdateBatchForTest(Batch);
	};

	// A row drawn from updates alone has no record here, so the destroy reaches it through the manager.
	const FGuid Row = FGuid::NewGuid();
	Fixture.Tracker->OnRemoteEntityAppeared.Broadcast(Row, MakeAppearanceState(1.0), 1);
	TestEqual(TEXT("the row is drawn"), Rows->ActivatedSlots.Num(), 1);

	FCrowdyEntityDestroyEvent Destroy;
	Destroy.EntityID = Row;
	Fixture.Entities->HandleRemoteDestroyForTest(Destroy);
	if (TestEqual(TEXT("the destroy event reaches the backend once"), Rows->ReleasedOnDestroy.Num(), 1))
	{
		TestEqual(TEXT("naming the row's slot"), Rows->ReleasedOnDestroy[0].Key, Rows->ActivatedSlots[0]);
		TestEqual(TEXT("and its id"), Rows->ReleasedOnDestroy[0].Value, Row);
	}

	Update(Row);
	TestEqual(TEXT("an update still in flight does not draw it again"), Rows->ActivatedSlots.Num(), 1);

	// An entity that keeps sending after the window is alive, as after a forged destroy: it is drawn again.
	FPlatformProcess::Sleep(static_cast<float>(UCrowdyEntitySubsystem::RecentlyDestroyedSeconds) + 0.1f);
	Update(Row);
	TestEqual(TEXT("an update after the window draws it again"), Rows->ActivatedSlots.Num(), 2);

	const FGuid Unknown = FGuid::NewGuid();
	Destroy.EntityID = Unknown;
	Fixture.Entities->HandleRemoteDestroyForTest(Destroy);
	if (TestEqual(TEXT("a destroy for an id with no slot still reaches the backend"), Rows->ReleasedOnDestroy.Num(), 2))
	{
		TestEqual(TEXT("with no slot"), Rows->ReleasedOnDestroy[1].Key, static_cast<int32>(INDEX_NONE));
	}
	return true;
}

// A sign-in gives the player a new actor id while the connection is down. Every entity the old id owned is re-pointed
// to the new one without announcing a change of owner, keeps its update extras, and sends under the new owner's wire
// id. Once the connection is up it grants itself from the old id to the new one, so peers' compare-and-swap re-points
// it, announces its spawn again, and does all of that once more after the staleness window. Another player's entity
// is left alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRekeyedEntitiesReannounceTest,
	"CrowdySDK.CrowdyReplication.ARekeyRestampsAndReannouncesThePlayersSpawnedEntities", CrowdyAppearanceTestFlags)
bool FCrowdyRekeyedEntitiesReannounceTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	UCrowdyAutoReplicator* AutoReplicator = NewObject<UCrowdyAutoReplicator>(TestWorld.World);
	const TStrongObjectPtr<UCrowdyEntitySubsystem> Entities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
	UCrowdyGameSession* Session = MakeGameSession(FGuid());
	Entities->SetGameSessionForTest(Session);
	Entities->BindGameSessionForTest();
	const FGuid Old = FGuid::NewGuid();
	Session->SetUUID(Old.ToString(EGuidFormats::Digits));

	auto MakeEntity = [&](UClass* Class, const FGuid& Owner, const ECrowdyRole Role) -> UCrowdyEntityComponent*
	{
		AActor* Actor = TestWorld.World->SpawnActor<AActor>(Class);
		UCrowdyEntityComponent* Component = Actor ? Actor->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
		if (!Component)
			return nullptr;

		FCrowdyEntityRecord Record;
		Record.NetID = FGuid::NewGuid();
		Record.WriterID = Owner;
		Record.Role = Role;
		Record.Participant = Actor;
		Entities->RegisterEntity(Record);

		Component->SetAutoReplicatorForTest(AutoReplicator);
		Component->SetCollaboratorsForTest(Entities.Get(), nullptr);
		Component->AssignPooledIdentity(Record.NetID, Owner, 0, Role, 0);
		Component->DeliverSpawnData(MakeAppearanceState(3.0), true);
		return Component;
	};

	const FGuid Peer = FGuid::NewGuid();
	UCrowdyEntityComponent* Still = MakeEntity(ACrowdyStateHostOverrideActor::StaticClass(), Old, ECrowdyRole::Owner);
	UCrowdyEntityComponent* Moving = MakeEntity(ACrowdyStateOverlapActor::StaticClass(), Old, ECrowdyRole::Owner);
	UCrowdyEntityComponent* Theirs = MakeEntity(ACrowdyStateHostOverrideActor::StaticClass(), Peer, ECrowdyRole::RemoteProxy);
	if (!TestNotNull(TEXT("still"), Still) || !TestNotNull(TEXT("moving"), Moving) || !TestNotNull(TEXT("theirs"), Theirs))
	{
		return false;
	}
	Still->EnablePresence();
	AutoReplicator->RegisterReplicationComponent(Moving, /*bPresenceOnly=*/false);
	const TArray<uint8> Extras = { 1, 0, 2, 0, 0xAA, 0xBB };
	TestTrue(TEXT("the mover carries extras the game set"), AutoReplicator->SetExtras(Moving, Extras));
	const int32 StillAnnouncements = Still->OwnershipAnnouncementCount;
	const int32 MovingAnnouncements = Moving->OwnershipAnnouncementCount;

	const FGuid New = FGuid::NewGuid();
	Session->SetUUID(New.ToString(EGuidFormats::Digits));

	for (const UCrowdyEntityComponent* Component : {Still, Moving})
	{
		const FCrowdyEntityRecord* Record = Entities->FindRecord(Component->GetNetID());
		TestTrue(TEXT("the record names the new id"), Record && Record->WriterID == New && Record->Role == ECrowdyRole::Owner);
		TestEqual(TEXT("and so does the component"), Component->GetWriterID(), New);
		TestEqual(TEXT("which sends under the new owner's wire id"), Component->GetReplicationUUID(),
			FCrowdyActorId::ForOwner(Component->GetNetID(), New).ToString());
		TestEqual(TEXT("and so does its replication entry"), AutoReplicator->GetSendIdForTest(Component), Component->GetReplicationUUID());
		TestTrue(TEXT("and still writes"), Entities->IsLocallyOwned(Component->GetNetID()));
	}
	TestEqual(TEXT("the player still owns them, so no change of owner is announced"), Still->OwnershipAnnouncementCount, StillAnnouncements);
	TestEqual(TEXT("for either"), Moving->OwnershipAnnouncementCount, MovingAnnouncements);
	TestTrue(TEXT("the mover keeps its extras"), AutoReplicator->GetExtrasForTest(Moving) == Extras);
	bool bPresenceOnly = false;
	TestTrue(TEXT("a still entity keeps sending presence"), AutoReplicator->IsRegisteredForTest(Still, bPresenceOnly) && bPresenceOnly);
	TestEqual(TEXT("another player's entity keeps its owner"), Theirs->GetWriterID(), Peer);
	TestTrue(TEXT("the old id is still this client's, so its echo is not a peer's"), Entities->IsLocalSender(Old));
	TestFalse(TEXT("unlike a peer's"), Entities->IsLocalSender(Peer));

	// Nothing reaches anyone while the connection is down, so the announcements wait for it.
	TestEqual(TEXT("both wait for the connection"), Entities->GetRekeyedAwaitingConnectionCountForTest(), 2);
	Entities->bCaptureAnnouncedSpawnsForTest = true;
	Entities->HandleRealtimeUpForTest();
	if (!TestEqual(TEXT("once it is up, each announces its spawn again"), Entities->AnnouncedSpawnsForTest.Num(), 2))
	{
		return false;
	}
	for (const FCrowdyEntitySpawnEvent& Sent : Entities->AnnouncedSpawnsForTest)
	{
		TestEqual(TEXT("naming the new id as its owner"), Sent.WriterID, New);
		TestEqual(TEXT("with the spawn data it was given"), AppearanceStateX(Sent.InitialState), 3.0);
	}
	if (!TestEqual(TEXT("and each grants itself to the new id"), Entities->RekeyGrantsForTest.Num(), 2))
	{
		return false;
	}
	for (const FCrowdyRekeyedID& Grant : Entities->RekeyGrantsForTest)
	{
		TestTrue(TEXT("from the old id peers hold, so their compare-and-swap takes it"), Grant.PreviousID == Old && Grant.NewID == New);
	}

	// What a peer holding the entity under the old owner does with that grant.
	const TStrongObjectPtr<UCrowdyEntitySubsystem> PeerEntities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
	PeerEntities->SetGameSessionForTest(MakeGameSession(Peer));
	PeerEntities->BindGameSessionForTest();
	FCrowdyEntityRecord PeerRecord;
	PeerRecord.NetID = Moving->GetNetID();
	PeerRecord.WriterID = Old;
	PeerRecord.Role = ECrowdyRole::RemoteProxy;
	PeerEntities->RegisterEntity(PeerRecord);
	PeerEntities->ReassignOwnership(Moving->GetNetID(), Entities->RekeyGrantsForTest[0].NewID, Entities->RekeyGrantsForTest[0].PreviousID,
		Entities->RekeyGrantUsersForTest[0]);
	TestEqual(TEXT("a peer re-points the entity to the new owner"), PeerEntities->FindRecord(Moving->GetNetID())->WriterID, New);

	Entities->AnnouncedSpawnsForTest.Reset();
	Entities->RekeyGrantsForTest.Reset();
	Entities->HandleRealtimeUpForTest();
	TestEqual(TEXT("and only once per connection"), Entities->AnnouncedSpawnsForTest.Num(), 0);

	TestTrue(TEXT("a repeat waits for the staleness window"), Entities->RunRekeyRepeatForTest());
	TestEqual(TEXT("and announces each again"), Entities->AnnouncedSpawnsForTest.Num(), 2);
	TestEqual(TEXT("with its grant"), Entities->RekeyGrantsForTest.Num(), 2);
	TestFalse(TEXT("once"), Entities->RunRekeyRepeatForTest());
	return true;
}

// Two re-keys inside the repeat's delay: a peer that missed both announcements still holds the first owner, so the one
// repeat names that one. The owner's own wire id kept since the entity changed hands follows each re-key too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRekeyRepeatKeepsTheFirstOwnerTest,
	"CrowdySDK.CrowdyReplication.ARepeatAfterTwoRekeysGrantsFromTheFirstPreviousOwner", CrowdyAppearanceTestFlags)
bool FCrowdyRekeyRepeatKeepsTheFirstOwnerTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	UCrowdyAutoReplicator* AutoReplicator = NewObject<UCrowdyAutoReplicator>(TestWorld.World);
	const TStrongObjectPtr<UCrowdyEntitySubsystem> Entities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	UCrowdyGameSession* Session = MakeGameSession(FGuid());
	Entities->SetGameSessionForTest(Session);
	Entities->BindGameSessionForTest();
	Entities->ActorTrackerForTest = Tracker.Get();
	const FGuid Old = FGuid::NewGuid();
	Session->SetUUID(Old.ToString(EGuidFormats::Digits));

	AActor* Actor = TestWorld.World->SpawnActor<AActor>(ACrowdyStateOverlapActor::StaticClass());
	UCrowdyEntityComponent* Component = Actor ? Actor->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
	if (!TestNotNull(TEXT("the entity spawned"), Component))
	{
		return false;
	}
	FCrowdyEntityRecord Record;
	Record.NetID = FGuid::NewGuid();
	Record.WriterID = Old;
	Record.Role = ECrowdyRole::Owner;
	Record.Participant = Actor;
	Entities->RegisterEntity(Record);
	Component->SetAutoReplicatorForTest(AutoReplicator);
	Component->SetCollaboratorsForTest(Entities.Get(), nullptr);
	Component->AssignPooledIdentity(Record.NetID, Old, 0, ECrowdyRole::Owner, 0);
	Component->DeliverSpawnData(MakeAppearanceState(3.0), true);

	// It came to this player from someone else earlier, so the tracker keeps the wire id it moved to.
	Tracker->HandleEntityOwnerChangedForTest(Record.NetID, Old);
	Entities->bCaptureAnnouncedSpawnsForTest = true;

	const FGuid Mid = FGuid::NewGuid();
	Session->SetUUID(Mid.ToString(EGuidFormats::Digits));
	Entities->HandleRealtimeUpForTest();
	const FGuid New = FGuid::NewGuid();
	Session->SetUUID(New.ToString(EGuidFormats::Digits));
	Entities->HandleRealtimeUpForTest();

	FCrowdyActorId Held;
	TestTrue(TEXT("the kept wire id follows the re-key"), Tracker->FindWireId(Record.NetID, Held)
		&& Held == FCrowdyActorId::ForOwner(Record.NetID, New));

	Entities->RekeyGrantsForTest.Reset();
	if (!TestTrue(TEXT("one repeat waits"), Entities->RunRekeyRepeatForTest())
		|| !TestEqual(TEXT("with one grant"), Entities->RekeyGrantsForTest.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("from the first previous owner"), Entities->RekeyGrantsForTest[0].PreviousID, Old);
	TestEqual(TEXT("to the newest id"), Entities->RekeyGrantsForTest[0].NewID, New);
	return true;
}

// Another user signs in on this client: the re-key's grant names the user signed in when the connection comes up, so
// a peer holding the entity under the previous user's claim learns the new one along with the new writer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRekeyAfterUserChangeNamesNewUserTest,
	"CrowdySDK.CrowdyReplication.ARekeyAfterAUserChangeNamesTheNewUser", CrowdyAppearanceTestFlags)
bool FCrowdyRekeyAfterUserChangeNamesNewUserTest::RunTest(const FString& Parameters)
{
	constexpr int64 PreviousUser = 6001;
	constexpr int64 NextUser = 6002;
	FCrowdyAppearanceTestWorld TestWorld;
	UCrowdyAutoReplicator* AutoReplicator = NewObject<UCrowdyAutoReplicator>(TestWorld.World);
	const TStrongObjectPtr<UCrowdyEntitySubsystem> Entities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
	UCrowdyGameSession* Session = MakeGameSession(FGuid());
	Session->SetUserID(PreviousUser);
	Entities->SetGameSessionForTest(Session);
	Entities->BindGameSessionForTest();
	const FGuid Old = FGuid::NewGuid();
	Session->SetUUID(Old.ToString(EGuidFormats::Digits));

	AActor* Actor = TestWorld.World->SpawnActor<AActor>(ACrowdyStateHostOverrideActor::StaticClass());
	UCrowdyEntityComponent* Component = Actor ? Actor->FindComponentByClass<UCrowdyEntityComponent>() : nullptr;
	if (!TestNotNull(TEXT("the entity spawned"), Component))
	{
		return false;
	}
	FCrowdyEntityRecord Record;
	Record.NetID = FGuid::NewGuid();
	Record.WriterID = Old;
	Record.Role = ECrowdyRole::Owner;
	Record.Participant = Actor;
	Entities->RegisterEntity(Record);
	Component->SetAutoReplicatorForTest(AutoReplicator);
	Component->SetCollaboratorsForTest(Entities.Get(), nullptr);
	Component->AssignPooledIdentity(Record.NetID, Old, 0, ECrowdyRole::Owner, 0);
	Component->DeliverSpawnData(MakeAppearanceState(3.0), true);
	Entities->bCaptureAnnouncedSpawnsForTest = true;

	Session->SetUserID(NextUser);
	const FGuid New = FGuid::NewGuid();
	Session->SetUUID(New.ToString(EGuidFormats::Digits));
	Entities->HandleRealtimeUpForTest();

	if (!TestEqual(TEXT("the re-key grants the entity to the new id"), Entities->RekeyGrantsForTest.Num(), 1)
		|| !TestEqual(TEXT("naming one user"), Entities->RekeyGrantUsersForTest.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("the user signed in now"), Entities->RekeyGrantUsersForTest[0], NextUser);
	TestTrue(TEXT("and the spawn it announces again names that user too"),
		Entities->AnnouncedSpawnsForTest.Num() == 1 && Entities->AnnouncedSpawnsForTest[0].OwnerUserID == NextUser);

	// A peer holding the entity under the old writer and the previous user's claim.
	const TStrongObjectPtr<UCrowdyEntitySubsystem> PeerEntities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
	PeerEntities->SetLocalPlayerID(FGuid::NewGuid());
	FCrowdyEntityRecord PeerRecord;
	PeerRecord.NetID = Record.NetID;
	PeerRecord.WriterID = Old;
	PeerRecord.OwnerUserID = PreviousUser;
	PeerRecord.Role = ECrowdyRole::RemoteProxy;
	PeerEntities->RegisterEntity(PeerRecord);
	PeerEntities->ReassignOwnership(Record.NetID, Entities->RekeyGrantsForTest[0].NewID, Entities->RekeyGrantsForTest[0].PreviousID,
		Entities->RekeyGrantUsersForTest[0]);
	const FCrowdyEntityRecord* Held = PeerEntities->FindRecord(Record.NetID);
	TestTrue(TEXT("the peer follows the new writer"), Held && Held->WriterID == New);
	TestTrue(TEXT("and learns the new user"), Held && PeerEntities->GetOwnerUserID(*Held) == NextUser);
	return true;
}

// An owner-salted participant re-derives its id when the player's id changes, and whoever registered it holds the old
// one: the re-key notification names both, so that holder can follow.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRekeyNamesParticipantIdsTest,
	"CrowdySDK.CrowdyReplication.ARekeyNamesEachParticipantsOldAndNewId", CrowdyAppearanceTestFlags)
bool FCrowdyRekeyNamesParticipantIdsTest::RunTest(const FString& Parameters)
{
	FCrowdyAppearanceTestWorld TestWorld;
	const TStrongObjectPtr<UCrowdyEntitySubsystem> Entities(NewObject<UCrowdyEntitySubsystem>(TestWorld.World));
	UCrowdyGameSession* Session = MakeGameSession(FGuid());
	Entities->SetGameSessionForTest(Session);
	Entities->BindGameSessionForTest();
	Session->SetUUID(FGuid::NewGuid().ToString(EGuidFormats::Digits));

	// Any object stands in for the subsystem that would enrol itself.
	const TStrongObjectPtr<UGameInstance> Holder(NewObject<UGameInstance>(GetTransientPackage()));
	UObject* Participant = Holder.Get();
	const FGuid Registered = Entities->RegisterParticipant(Participant, ECrowdyOwnership::LocalClient);
	if (!TestTrue(TEXT("the participant registered under an owner-salted id"), Registered.IsValid()))
	{
		return false;
	}

	TArray<FCrowdyRekeyedID> Heard;
	Entities->OnLocalPlayerRekeyed.AddLambda([&Heard](const FGuid&, const FGuid&, TConstArrayView<FCrowdyRekeyedID> Participants)
	{
		Heard.Append(Participants.GetData(), Participants.Num());
	});
	Session->SetUUID(FGuid::NewGuid().ToString(EGuidFormats::Digits));

	if (!TestEqual(TEXT("the notification names the participant"), Heard.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("by the id its holder registered"), Heard[0].PreviousID, Registered);
	TestTrue(TEXT("and the one it answers to now"), Heard[0].NewID.IsValid() && Heard[0].NewID != Registered
		&& Entities->FindEntityID(Participant) == Heard[0].NewID);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
