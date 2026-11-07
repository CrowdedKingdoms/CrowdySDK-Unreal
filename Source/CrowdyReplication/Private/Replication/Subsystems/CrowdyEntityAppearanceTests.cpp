// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyActorPoolBackend.h"
#include "Data/CrowdyActorPoolBackendConfig.h"
#include "Data/CrowdyEntityTypes.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
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
			Event.OwnerID = FGuid::NewGuid();
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
	TestEqual(TEXT("owned by the Leader"), Record->OwnerID, Leader);

	const UCrowdyEntityComponent* Component = Record->GetActor()->FindComponentByClass<UCrowdyEntityComponent>();
	TestEqual(TEXT("and its component agrees"), Component->GetOwnerID(), Leader);
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
		void Know(const FGuid& UUID, const FGuid& OwnerID = FGuid())
		{
			UObject* Participant = NewObject<UCrowdySpawnDataListener>(GetTransientPackage());
			Participants.Emplace(Participant);

			FCrowdyEntityRecord Record;
			Record.NetID = UUID;
			Record.OwnerID = OwnerID;
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
	Fixture.Entities->ReassignOwnership(Given, FGuid::NewGuid(), FGuid());
	Fixture.Update(Given);
	TestEqual(TEXT("ownership leaving this client sets the slot up again"), Fixture.Backend->ActivatedSlots.Num(), 3);

	Fixture.Entities->ReassignOwnership(Elsewhere, FGuid::NewGuid(), FGuid());
	Fixture.Update(Elsewhere);
	TestEqual(TEXT("a hand-over between two other clients leaves the slot alone"), Fixture.Backend->ActivatedSlots.Num(), 3);

	Fixture.Entities->ReassignOwnership(Elsewhere, LocalPlayer, FGuid());
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
	OwnerRecord.OwnerID = LocalPlayer;
	OwnerRecord.Role = ECrowdyRole::Owner;
	OwnerRecord.Participant = OwnedActor;
	Fixture.Entities->RegisterEntity(OwnerRecord);

	Fixture.Activate(0, Owned);
	if (!TestNull(TEXT("the entity owned here is not drawn by the pool"), Fixture.Backend->GetSlotActorForTest(0)))
	{
		return false;
	}

	// Handed to another client: its updates now move this client's actor, which the slot adopts.
	Fixture.Entities->ReassignOwnership(Owned, FGuid::NewGuid(), FGuid());
	Fixture.Activate(0, Owned);
	TestTrue(TEXT("once handed away, the actor is drawn from the new owner's updates"), Fixture.Backend->GetSlotActorForTest(0) == OwnedActor);
	TestTrue(TEXT("and the slot is active"), Fixture.Backend->IsInstanceActive(0));

	// Handed back: the slot lets go of the actor without destroying it, since this client moves it again.
	Fixture.Entities->ReassignOwnership(Owned, LocalPlayer, FGuid());
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
		Record.OwnerID = Role == ECrowdyRole::Owner ? LocalPlayer : FGuid::NewGuid();
		Record.Role = Role;
		Record.Participant = Actor;
		Entities->RegisterEntity(Record);

		Component->SetAutoReplicatorForTest(AutoReplicator);
		Component->SetCollaboratorsForTest(Entities.Get(), nullptr);
		Component->AssignPooledIdentity(Record.NetID, Record.OwnerID, Role, 0);
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
	Entities->ReassignOwnership(Mirrored->GetNetID(), LocalPlayer, FGuid());
	TestTrue(TEXT("a mirrored entity handed to this client starts sending presence"), AutoReplicator->IsRegisteredForTest(Mirrored, bPresenceOnly) && bPresenceOnly);
	Entities->ReassignOwnership(Owned->GetNetID(), FGuid::NewGuid(), FGuid());
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
		TestEqual(TEXT("naming this client, its owner now"), Sent.OwnerID, LocalPlayer);
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
			TestEqual(TEXT("that knows its owner"), Built->GetOwnerID(), LocalPlayer);
			TestTrue(TEXT("where the entity is now"), Built->GetOwner()->GetActorLocation().Equals(FVector(300.0, 40.0, 5.0)));
		}
	}

	// A repeat names the owner the entity has, not whoever sends it: the first entity was handed to another player.
	Entities->AnnouncedSpawnsForTest.Reset();
	Owned->SendPresence();
	if (TestEqual(TEXT("a send for the handed-away entity announces once"), Entities->AnnouncedSpawnsForTest.Num(), 1))
	{
		TestEqual(TEXT("naming the entity's own owner, not this client"), Entities->AnnouncedSpawnsForTest[0].OwnerID, Owned->GetOwnerID());
		TestNotEqual(TEXT("which is another player"), Entities->AnnouncedSpawnsForTest[0].OwnerID, LocalPlayer);
	}

	// Destroying it stops the sends at once, not when its Destroy Delay runs out and the actor goes.
	Mirrored->DestroyDelay = 5.f;
	Entities->DestroyEntity(Mirrored->GetOwner());
	TestFalse(TEXT("a destroyed entity stops sending at once"), AutoReplicator->IsRegisteredForTest(Mirrored, bPresenceOnly));
	TestTrue(TEXT("and the destroying client remembers it, so a repeat already on its way cannot rebuild it"),
		Entities->WasRecentlyDestroyed(Mirrored->GetNetID()));
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
	TestFalse(TEXT("which does not know its owner yet"), Component->GetOwnerID().IsValid());

	FCrowdyEntitySpawnEvent Event;
	Event.EntityID = Entity;
	Event.OwnerID = FGuid::NewGuid();
	Event.InitialState = MakeAppearanceState(3.0);
	Fixture.Entities->HandleRemoteSpawnForTest(Event);

	TestEqual(TEXT("the spawn event names the owner on the record"), Fixture.Entities->FindRecord(Entity)->OwnerID, Event.OwnerID);
	TestEqual(TEXT("and on the actor already drawn for it"), Component->GetOwnerID(), Event.OwnerID);
	TestEqual(TEXT("which keeps its id and role"), Component->GetNetID(), Entity);
	TestEqual(TEXT("as a remote copy"), Component->GetRole(), ECrowdyRole::RemoteProxy);
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
			Event.OwnerID = FGuid::NewGuid();
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

#endif // WITH_DEV_AUTOMATION_TESTS
