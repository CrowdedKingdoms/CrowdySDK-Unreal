// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/AutomationTest.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// A backend that could not activate an instance (a pool at its cap with nothing to adopt) says so through
// IsInstanceActive, and the manager activates it again instead of feeding a slot with nothing in it until the entity
// times out: on the entity's next update once something may have freed capacity, or at once when the entity registers.
namespace
{
	constexpr EAutomationTestFlags CrowdyActivationRetryTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FInstancedStruct MakeRetryState(const double X)
	{
		FCrowdyActorState Value;
		Value.Location = FVector(X, 0.0, 0.0);
		return FInstancedStruct::Make(Value);
	}

	double ReadRetryX(const FInstancedStruct& State)
	{
		const FCrowdyActorState* Typed = State.GetPtr<FCrowdyActorState>();
		return Typed ? Typed->Location.X : -1.0;
	}

	/** A tracker and a manager wired the way Initialize wires them, with a backend that records what it was asked. */
	struct FCrowdyRetryFixture
	{
		TStrongObjectPtr<UCrowdyActorTracker> Tracker;
		TStrongObjectPtr<UCrowdyActorManager> Manager;
		TStrongObjectPtr<UCrowdyRecordingBackend> Backend;

		FCrowdyRetryFixture()
			: Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()))
			, Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()))
			, Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()))
		{
			Manager->SetBackend(Backend.Get());
			Manager->BindToTrackerForTest(Tracker.Get());
			Manager->RegisterStateClass(FCrowdyActorState::StaticStruct(), AActor::StaticClass());
		}

		void Appear(const FGuid& UUID, const double X) const
		{
			Tracker->OnRemoteEntityAppeared.Broadcast(UUID, MakeRetryState(X), 1);
		}

		void Update(const FGuid& UUID, const double X) const
		{
			FCrowdyActorUpdate Update;
			Update.UUID = UUID;
			Update.ServerTimestamp = 1000;
			Update.State = MakeRetryState(X);

			TArray<FCrowdyActorUpdate> Batch;
			Batch.Add(Update);
			Manager->DrainUpdateBatchForTest(Batch);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInactiveSlotRetriedAfterCapacityFreedTest,
	"CrowdySDK.ActorManager.InactiveSlotRetriedAfterCapacityFreed", CrowdyActivationRetryTestFlags)
bool FCrowdyInactiveSlotRetriedAfterCapacityFreedTest::RunTest(const FString& Parameters)
{
	const FCrowdyRetryFixture Fixture;

	// The control, activated while the backend still answers active, so it is never retried below even once the
	// backend starts answering inactive: the manager asks once per activation, not once per update.
	const FGuid Active = FGuid::NewGuid();
	Fixture.Appear(Active, 0.0);
	const FGuid Leaving = FGuid::NewGuid();
	Fixture.Appear(Leaving, 0.5);

	Fixture.Backend->bReportInactive = true;
	const FGuid Refused = FGuid::NewGuid();
	Fixture.Appear(Refused, 1.0);
	TestEqual(TEXT("every entity was activated once"), Fixture.Backend->ActivatedSlots.Num(), 3);

	Fixture.Update(Refused, 2.0);
	Fixture.Update(Refused, 2.5);
	TestEqual(TEXT("no retry while nothing has freed capacity"), Fixture.Backend->ActivatedSlots.Num(), 3);

	Fixture.Tracker->OnRemoteEntityTimedOut.Broadcast(Leaving, 1);
	Fixture.Update(Refused, 3.0);
	Fixture.Update(Active, 3.0);
	TestEqual(TEXT("a released slot lets the inactive entity's next update retry, and only it"), Fixture.Backend->ActivatedSlots.Num(), 4);
	if (Fixture.Backend->ActivatedStates.Num() == 4)
	{
		TestEqual(TEXT("with that update's state"), ReadRetryX(Fixture.Backend->ActivatedStates[3]), 3.0);
	}

	Fixture.Update(Refused, 3.5);
	TestEqual(TEXT("a retry that fails again waits for the next freed capacity"), Fixture.Backend->ActivatedSlots.Num(), 4);

	// Any entity leaving the registry may have been a destroyed pool actor, so it counts as freed capacity too.
	Fixture.Backend->bReportInactive = false;
	Fixture.Manager->NotifyEntityUnregisteredForTest(FGuid::NewGuid());
	Fixture.Update(Refused, 4.0);
	TestEqual(TEXT("an unregistration lets the next update retry"), Fixture.Backend->ActivatedSlots.Num(), 5);

	Fixture.Update(Refused, 4.5);
	TestEqual(TEXT("and once active the entity is not activated again"), Fixture.Backend->ActivatedSlots.Num(), 5);
	TestEqual(TEXT("every update still reached the backend"), Fixture.Backend->ExtractedStates.Num(), 3 + 7);
	return true;
}

// The same retry for an entity that was activated late, when its spawn event resolved a parked entry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInactiveAfterParkRetriedTest,
	"CrowdySDK.ActorManager.InactiveAfterParkRetriedAfterCapacityFreed", CrowdyActivationRetryTestFlags)
bool FCrowdyInactiveAfterParkRetriedTest::RunTest(const FString& Parameters)
{
	const FCrowdyRetryFixture Fixture;
	Fixture.Backend->bReportInactive = true;

	const FGuid Parked = FGuid::NewGuid();
	Fixture.Manager->ParkActivationForTest(Parked, MakeRetryState(1.0));
	Fixture.Manager->CompletePendingActivationForTest(Parked);
	TestEqual(TEXT("the parked entry was activated when its entity registered"), Fixture.Backend->ActivatedSlots.Num(), 1);

	Fixture.Manager->NotifyEntityUnregisteredForTest(FGuid::NewGuid());
	Fixture.Update(Parked, 2.0);
	TestEqual(TEXT("an inactive result there is retried like any other"), Fixture.Backend->ActivatedSlots.Num(), 2);
	return true;
}

// The entity's spawn-event actor registering is what a slot at the pool's cap is waiting for, so it retries at once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInactiveSlotRetriedOnRegistrationTest,
	"CrowdySDK.ActorManager.InactiveSlotRetriedOnRegistration", CrowdyActivationRetryTestFlags)
bool FCrowdyInactiveSlotRetriedOnRegistrationTest::RunTest(const FString& Parameters)
{
	const FCrowdyRetryFixture Fixture;

	const FGuid Active = FGuid::NewGuid();
	Fixture.Appear(Active, 0.0);

	Fixture.Backend->bReportInactive = true;
	const FGuid Refused = FGuid::NewGuid();
	Fixture.Appear(Refused, 1.0);

	Fixture.Manager->CompletePendingActivationForTest(Active);
	TestEqual(TEXT("an active entity registering is not activated again"), Fixture.Backend->ActivatedSlots.Num(), 2);

	Fixture.Manager->CompletePendingActivationForTest(Refused);
	TestEqual(TEXT("an inactive entity registering is activated at once, with no update and no freed capacity"),
		Fixture.Backend->ActivatedSlots.Num(), 3);
	return true;
}

// A retry uses the class the slot was first activated with. Resolving it again on every update of a slot at the
// pool's cap cost a registry lookup, and a warning when the class id did not resolve, per update.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRetryUsesFirstActivationClassTest,
	"CrowdySDK.ActorManager.RetryUsesFirstActivationClass", CrowdyActivationRetryTestFlags)
bool FCrowdyRetryUsesFirstActivationClassTest::RunTest(const FString& Parameters)
{
	const FCrowdyRetryFixture Fixture;
	Fixture.Backend->bReportInactive = true;

	const FGuid Refused = FGuid::NewGuid();
	Fixture.Appear(Refused, 1.0);

	// What a fresh resolve would now answer for the same state.
	Fixture.Manager->RegisterStateClass(FCrowdyActorState::StaticStruct(), APawn::StaticClass());

	Fixture.Manager->NotifyEntityUnregisteredForTest(FGuid::NewGuid());
	Fixture.Update(Refused, 2.0);
	Fixture.Manager->CompletePendingActivationForTest(Refused);
	if (!TestEqual(TEXT("the entity was activated three times"), Fixture.Backend->ActivatedClasses.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("first with the resolved class"), Fixture.Backend->ActivatedClasses[0] == AActor::StaticClass());
	TestTrue(TEXT("the update's retry with that same class"), Fixture.Backend->ActivatedClasses[1] == AActor::StaticClass());
	TestTrue(TEXT("the registration's retry with that same class"), Fixture.Backend->ActivatedClasses[2] == AActor::StaticClass());
	return true;
}

// A remote destroy takes the actor from under an active slot. The backend then answers inactive and the entity's next
// update activates the slot again: a respawned avatar keeps its id and keeps sending.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUnregisteredActiveSlotRetriedTest,
	"CrowdySDK.ActorManager.UnregisteredActiveSlotRetried", CrowdyActivationRetryTestFlags)
bool FCrowdyUnregisteredActiveSlotRetriedTest::RunTest(const FString& Parameters)
{
	const FCrowdyRetryFixture Fixture;

	const FGuid Destroyed = FGuid::NewGuid();
	Fixture.Appear(Destroyed, 0.0);

	// The control: a backend still drawing the slot after the unregistration (Mass, say) is left alone.
	Fixture.Manager->NotifyEntityUnregisteredForTest(Destroyed);
	Fixture.Update(Destroyed, 1.0);
	TestEqual(TEXT("an unregistration the backend still draws through is not an activation"), Fixture.Backend->ActivatedSlots.Num(), 1);

	Fixture.Backend->bReportInactive = true;
	Fixture.Manager->NotifyEntityUnregisteredForTest(Destroyed);
	Fixture.Backend->bReportInactive = false;
	Fixture.Update(Destroyed, 2.0);
	TestEqual(TEXT("a slot the unregistration emptied is activated by the next update"), Fixture.Backend->ActivatedSlots.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyReleasedSlotClearsInactiveBitTest,
	"CrowdySDK.ActorManager.ReleasedSlotClearsInactiveBit", CrowdyActivationRetryTestFlags)
bool FCrowdyReleasedSlotClearsInactiveBitTest::RunTest(const FString& Parameters)
{
	const FCrowdyRetryFixture Fixture;
	Fixture.Backend->bReportInactive = true;

	const FGuid Refused = FGuid::NewGuid();
	Fixture.Appear(Refused, 1.0);
	if (!TestEqual(TEXT("the entity was activated once"), Fixture.Backend->ActivatedSlots.Num(), 1))
	{
		return false;
	}
	const int32 Slot = Fixture.Backend->ActivatedSlots[0];

	Fixture.Tracker->OnRemoteEntityTimedOut.Broadcast(Refused, 1);

	// The next holder of the slot is given it without an activation, as a parked entry is. It must not inherit the
	// departed entity's retry.
	Fixture.Backend->bReportInactive = false;
	const FGuid Next = FGuid::NewGuid();
	TestEqual(TEXT("the released slot is reused"), Fixture.Manager->AllocateSlotForTest(Next), Slot);

	Fixture.Update(Next, 2.0);
	TestEqual(TEXT("the slot's next holder is not activated by the departed entity's retry"), Fixture.Backend->ActivatedSlots.Num(), 1);
	return true;
}

// An activation registers the entity itself (the pool registers its actor, replacing the spawn-event actor), so the
// registration broadcast arrives while the slot is still flagged inactive. It must not start a second activation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRetryDoesNotReenterTest,
	"CrowdySDK.ActorManager.RetryDoesNotReenterOnItsOwnRegistration", CrowdyActivationRetryTestFlags)
bool FCrowdyRetryDoesNotReenterTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyEntitySubsystem> Entities(NewObject<UCrowdyEntitySubsystem>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRegisteringBackend> Backend(NewObject<UCrowdyRegisteringBackend>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRecordingBackend> SpawnEventActor(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));

	Backend->Entities = Entities.Get();
	Manager->SetBackend(Backend.Get());
	Manager->BindToTrackerForTest(Tracker.Get());
	Manager->BindToEntitySubsystemForTest(Entities.Get());
	Manager->RegisterStateClass(FCrowdyActorState::StaticStruct(), AActor::StaticClass());

	Backend->bReportInactive = true;
	const FGuid Refused = FGuid::NewGuid();
	Tracker->OnRemoteEntityAppeared.Broadcast(Refused, MakeRetryState(1.0), 1);
	TestEqual(TEXT("the entity was activated once"), Backend->ActivatedSlots.Num(), 1);

	// The spawn-event actor arriving: its registration retries the slot, and that retry registers again.
	Entities->UnregisterEntity(Refused);
	FCrowdyEntityRecord Record;
	Record.NetID = Refused;
	Record.Role = ECrowdyRole::RemoteProxy;
	Record.Participant = SpawnEventActor.Get();
	Entities->RegisterEntity(Record);

	TestEqual(TEXT("the registration retried the slot exactly once"), Backend->ActivatedSlots.Num(), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
