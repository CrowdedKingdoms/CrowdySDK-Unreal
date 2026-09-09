// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Misc/AutomationTest.h"
#include "Replication/Subsystems/CrowdyActorDepartureTestSupport.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/SerializationFunctionLibrary.h"

// A server-announced departure and a client-side staleness timeout are two different claims about the same
// actor, and the tracker reports whichever arrives first while suppressing the other. That makes the manager's
// two bindings a pair rather than a choice: an announced departure removes the actor from the map the timeout
// check reads, so an actor the server reported gone is never reported again by anything, and a manager bound
// only to the timeout would hold that actor's render slot for the life of the world.
//
// These cases pair a real tracker with a real manager and drive the departure through the delivery the router
// builds, so what they exercise is the wiring the game gets. The manager is bound through its own BindToTracker
// and never by hand here: a case that bound the delegates itself would keep passing with the binding deleted.
namespace
{
	constexpr EAutomationTestFlags CrowdyActorLeftReleaseTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** A tracker and a manager wired to each other the way Initialize wires them, with a backend that records. */
	struct FCrowdyActorLeftFixture
	{
		TStrongObjectPtr<UCrowdyActorTracker> Tracker;
		TStrongObjectPtr<UCrowdyActorManager> Manager;
		TStrongObjectPtr<UCrowdyRecordingBackend> Backend;

		FCrowdyActorLeftFixture()
			: Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()))
			, Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()))
			, Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()))
		{
			Manager->SetBackend(Backend.Get());
			Manager->BindToTrackerForTest(Tracker.Get());
		}

		bool IsValid() const { return Tracker.IsValid() && Manager.IsValid() && Backend.IsValid(); }

		/** Put an actor on screen: tracked by the tracker, holding a render slot in the manager. */
		int32 AddTrackedActor(const FGuid& UUID) const
		{
			Tracker->MarkActorTrackedForTest(UUID);
			return Manager->AllocateSlotForTest(UUID);
		}
	};
}

// The gate on the fix. Before it the manager bound only the timeout, so a server-announced departure erased the
// actor from the reaper's view and told nobody who could despawn it: the actor outlived the feature that was
// meant to retire it sooner.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAnnouncedLeaveReleasesTheSlotTest,
	"CrowdySDK.Replication.AnnouncedLeaveReleasesTheActorSlot", CrowdyActorLeftReleaseTestFlags)

bool FCrowdyAnnouncedLeaveReleasesTheSlotTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorLeftFixture Fixture;
	if (!TestTrue(TEXT("a tracker, a manager and a backend were created"), Fixture.IsValid()))
	{
		return false;
	}

	const FCrowdyActorId Leaver = CrowdyActorTestSender('a');
	const FCrowdyActorId Stayer = CrowdyActorTestSender('b');
	const FGuid LeaverGuid = USerializationFunctionLibrary::ToGuid(Leaver);
	const FGuid StayerGuid = USerializationFunctionLibrary::ToGuid(Stayer);
	if (!TestTrue(TEXT("the two actors derive different keys"), LeaverGuid != StayerGuid))
	{
		return false;
	}

	const int32 LeaverSlot = Fixture.AddTrackedActor(LeaverGuid);
	const int32 StayerSlot = Fixture.AddTrackedActor(StayerGuid);
	if (!TestNotEqual(TEXT("the two actors hold different slots"), LeaverSlot, StayerSlot))
	{
		return false;
	}
	TestEqual(TEXT("nothing has been released yet"), Fixture.Backend->DeactivatedSlots.Num(), 0);

	const TUniquePtr<FCrowdyActorTestDeliveryScope> Scope =
		CrowdyActorTestDeparture(Leaver, ECrowdyActorLeftReason::SessionReleased);
	if (!TestTrue(TEXT("a departure delivery was built"), Scope.IsValid()))
	{
		return false;
	}

	Fixture.Tracker->HandleActorLeftDelivery(*Scope->Delivery);

	// The backend's side, which is the only place a cleaned-up slot is visible. A counter kept beside the
	// manager would still read green with the DeactivateInstance call dropped.
	TestEqual(TEXT("exactly one slot was cleaned up"), Fixture.Backend->DeactivatedSlots.Num(), 1);
	TestTrue(TEXT("and it is the departing actor's"), Fixture.Backend->DeactivatedSlots.Contains(LeaverSlot));

	// The manager's own side, which is the half the backend cannot see: a slot the backend was told about but
	// that was never marked reusable would leak just as surely.
	const int32 NewcomerSlot = Fixture.Manager->AllocateSlotForTest(FGuid::NewGuid());
	TestEqual(TEXT("the departing actor's slot is handed to the next actor"), NewcomerSlot, LeaverSlot);

	// The control, which is what keeps both assertions above honest: a departure releases the actor it names
	// and no other. Without it, a release that cleared every slot would satisfy them.
	TestFalse(TEXT("the actor that stayed keeps its slot"),
		Fixture.Backend->DeactivatedSlots.Contains(StayerSlot));

	return true;
}

// The other half of the pair. The two departures are one release, and either binding going missing leaves the
// actors that arrive down that route holding their slots. Stated as both routes rather than as the new one,
// because the defect this guards against was adding a route and binding only the old one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBothDeparturesReleaseTheSlotTest,
	"CrowdySDK.Replication.BothDeparturesReleaseTheActorSlot", CrowdyActorLeftReleaseTestFlags)

bool FCrowdyBothDeparturesReleaseTheSlotTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorLeftFixture Fixture;
	if (!TestTrue(TEXT("a tracker, a manager and a backend were created"), Fixture.IsValid()))
	{
		return false;
	}

	const FCrowdyActorId Announced = CrowdyActorTestSender('c');
	const FGuid AnnouncedGuid = USerializationFunctionLibrary::ToGuid(Announced);
	const FGuid TimedOutGuid = FGuid::NewGuid();

	const int32 AnnouncedSlot = Fixture.AddTrackedActor(AnnouncedGuid);
	const int32 TimedOutSlot = Fixture.AddTrackedActor(TimedOutGuid);

	// The timeout route, driven through the delegate the tracker's own sweep broadcasts on.
	Fixture.Tracker->OnRemoteEntityTimedOut.Broadcast(TimedOutGuid, 1);
	TestTrue(TEXT("a timed-out actor's slot is released"),
		Fixture.Backend->DeactivatedSlots.Contains(TimedOutSlot));
	TestFalse(TEXT("and the other actor is untouched by it"),
		Fixture.Backend->DeactivatedSlots.Contains(AnnouncedSlot));

	// The announced route, through the delivery rather than the delegate, so the tracker decides which of its
	// two departure delegates fires. It only reaches OnRemoteEntityLeft for an actor this client was holding.
	const TUniquePtr<FCrowdyActorTestDeliveryScope> Scope = CrowdyActorTestDeparture(Announced);
	if (!TestTrue(TEXT("a departure delivery was built"), Scope.IsValid()))
	{
		return false;
	}

	Fixture.Tracker->HandleActorLeftDelivery(*Scope->Delivery);
	TestTrue(TEXT("an announced departure releases its actor's slot too"),
		Fixture.Backend->DeactivatedSlots.Contains(AnnouncedSlot));
	TestEqual(TEXT("and the two departures released two slots between them"),
		Fixture.Backend->DeactivatedSlots.Num(), 2);

	return true;
}

// A departure for an actor this client was never holding is suppressed on the reported delegate, so it must not
// reach the manager at all. The tracker allocates nothing for such an actor, so a release driven by it would be
// releasing a slot on the strength of a UUID the manager has no record of.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUntrackedLeaveReleasesNothingTest,
	"CrowdySDK.Replication.UntrackedLeaveReleasesNothing", CrowdyActorLeftReleaseTestFlags)

bool FCrowdyUntrackedLeaveReleasesNothingTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorLeftFixture Fixture;
	if (!TestTrue(TEXT("a tracker, a manager and a backend were created"), Fixture.IsValid()))
	{
		return false;
	}

	const FCrowdyActorId Stranger = CrowdyActorTestSender('d');
	const FGuid HeldGuid = FGuid::NewGuid();
	const int32 HeldSlot = Fixture.AddTrackedActor(HeldGuid);

	// This tracker was never told about the stranger, so it is holding nothing for it.
	const TUniquePtr<FCrowdyActorTestDeliveryScope> Scope = CrowdyActorTestDeparture(Stranger);
	if (!TestTrue(TEXT("a departure delivery was built"), Scope.IsValid()))
	{
		return false;
	}

	Fixture.Tracker->HandleActorLeftDelivery(*Scope->Delivery);

	TestEqual(TEXT("a departure for an actor never held releases nothing"),
		Fixture.Backend->DeactivatedSlots.Num(), 0);
	TestFalse(TEXT("and the actor that is held keeps its slot"),
		Fixture.Backend->DeactivatedSlots.Contains(HeldSlot));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
