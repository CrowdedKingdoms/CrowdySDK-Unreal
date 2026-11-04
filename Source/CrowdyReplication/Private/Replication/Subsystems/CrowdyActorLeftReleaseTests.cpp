// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Misc/AutomationTest.h"
#include "Replication/Subsystems/CrowdyActorDepartureTestSupport.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
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

// The server retires the old owner's id about 5 s after a move. That id's departure must not take the entity with it,
// and anything the old owner still sends under it is only that owner finishing up.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMovedEntitySurvivesItsOldIdTest,
	"CrowdySDK.Replication.MovedEntitySurvivesItsOldId", CrowdyActorLeftReleaseTestFlags)

bool FCrowdyMovedEntitySurvivesItsOldIdTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorLeftFixture Fixture;
	if (!TestTrue(TEXT("a tracker, a manager and a backend were created"), Fixture.IsValid()))
	{
		return false;
	}
	Fixture.Tracker->SetupQueuesForTest();

	const FGuid NetID = FGuid::NewGuid();
	const FGuid NewOwner = FGuid::NewGuid();
	const FCrowdyActorId OldId = FCrowdyActorId::FromGuid(NetID);
	const FCrowdyActorId NewId = FCrowdyActorId::ForOwner(NetID, NewOwner);
	const int32 Slot = Fixture.AddTrackedActor(NetID);

	Fixture.Tracker->HandleEntityOwnerChangedForTest(NetID, NewOwner);

	FCrowdyActorId Found;
	TestTrue(TEXT("the entity's id is known"), Fixture.Tracker->FindWireId(NetID, Found));
	TestTrue(TEXT("and a targeted message goes to its new owner's"), Found == NewId);

	const auto Deliver = [&Fixture, &NetID](const FCrowdyActorId& Id)
	{
		const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message = MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
		Message->GUID = NetID;
		Message->UUID = Id;
		const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe> Shared = Message;
		const FCrowdyDelivery Delivery(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION, Message->GetPayloadKey(),
			Message->GetPayload(), Message->GetPayloadBytes(), Shared);
		Fixture.Tracker->HandleActorUpdateDeliveryForTest(Delivery);
	};

	Deliver(OldId);
	TestEqual(TEXT("an update under the old id is dropped"), Fixture.Tracker->GetGatheredForTest().Num(), 0);
	Deliver(NewId);
	TestEqual(TEXT("an update under the new one is taken"), Fixture.Tracker->GetGatheredForTest().Num(), 1);

	const TUniquePtr<FCrowdyActorTestDeliveryScope> OldLeft = CrowdyActorTestDeparture(OldId, ECrowdyActorLeftReason::SessionReleased);
	const TUniquePtr<FCrowdyActorTestDeliveryScope> NewLeft = CrowdyActorTestDeparture(NewId, ECrowdyActorLeftReason::SessionReleased);
	if (!TestTrue(TEXT("departure deliveries were built"), OldLeft.IsValid() && NewLeft.IsValid()))
	{
		return false;
	}

	Fixture.Tracker->HandleActorLeftDelivery(*OldLeft->Delivery);
	TestEqual(TEXT("the old id leaving releases nothing"), Fixture.Backend->DeactivatedSlots.Num(), 0);

	Fixture.Tracker->HandleActorLeftDelivery(*NewLeft->Delivery);
	TestTrue(TEXT("the current id leaving releases the entity"), Fixture.Backend->DeactivatedSlots.Contains(Slot));

	Fixture.Tracker->HandleEntityUnregisteredForTest(NetID);
	Fixture.Tracker->MarkActorTrackedForTest(NetID);
	Deliver(OldId);
	TestEqual(TEXT("the move is forgotten with the entity"), Fixture.Tracker->GetGatheredForTest().Num(), 2);

	Fixture.Tracker->ReleaseQueuesForTest();
	return true;
}

// The targeted message follows the entity, except for an avatar, whose id nothing else can take over.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTargetedMessageFindsItsSenderTest,
	"CrowdySDK.Replication.ATargetedMessageGoesToTheIdItsSenderUses", CrowdyActorLeftReleaseTestFlags)

bool FCrowdyTargetedMessageFindsItsSenderTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorLeftFixture Fixture;
	const TStrongObjectPtr<UCrowdyEntitySubsystem> Entities(NewObject<UCrowdyEntitySubsystem>(GetTransientPackage()));
	const UCrowdyActorTracker* Tracker = Fixture.Tracker.Get();

	const auto Register = [&Entities, &Fixture](const FGuid& NetID, const FGuid& OwnerID, const bool bLeaderOwned)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = NetID;
		Record.OwnerID = OwnerID;
		Record.Role = ECrowdyRole::RemoteProxy;
		Record.bLeaderOwned = bLeaderOwned;
		Record.Participant = NewObject<UCrowdyRecordingBackend>(GetTransientPackage());
		Entities->RegisterEntity(Record);
		Fixture.Tracker->HandleEntityOwnerChangedForTest(NetID, FGuid::NewGuid());
	};

	const FGuid Moved = FGuid::NewGuid();
	Register(Moved, FGuid::NewGuid(), false);
	FCrowdyActorId Heard;
	Tracker->FindWireId(Moved, Heard);
	TestTrue(TEXT("an entity that changed hands is addressed by the id last heard"), Entities->ResolveTargetWireIdForTest(Moved, Tracker) == Heard);

	const FGuid Avatar = FGuid::NewGuid();
	Register(Avatar, Avatar, false);
	TestTrue(TEXT("an avatar always by its NetID"), Entities->ResolveTargetWireIdForTest(Avatar, Tracker) == FCrowdyActorId::FromGuid(Avatar));

	const FGuid Leader = FGuid::NewGuid();
	const FGuid LeaderOwner = FGuid::NewGuid();
	Register(Leader, LeaderOwner, true);
	TestTrue(TEXT("a Leader entity by its owner's id"), Entities->ResolveTargetWireIdForTest(Leader, Tracker) == FCrowdyActorId::ForOwner(Leader, LeaderOwner));

	const FGuid Unknown = FGuid::NewGuid();
	TestTrue(TEXT("and one never heard by its NetID"), Entities->ResolveTargetWireIdForTest(Unknown, Tracker) == FCrowdyActorId::FromGuid(Unknown));
	return true;
}

// Any client can make up an owner-tagged id that names someone else's entity, and the server will not stop it, since
// it has never seen that id. So a client already hearing the entity holds on to the id it hears it under.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInventedIdCannotSpeakForAnEntityTest,
	"CrowdySDK.Replication.AnInventedIdCannotSpeakForAnEntity", CrowdyActorLeftReleaseTestFlags)

bool FCrowdyInventedIdCannotSpeakForAnEntityTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorLeftFixture Fixture;
	if (!TestTrue(TEXT("a tracker, a manager and a backend were created"), Fixture.IsValid()))
	{
		return false;
	}
	Fixture.Tracker->SetupQueuesForTest();

	const FGuid NetID = FGuid::NewGuid();
	const FCrowdyActorId RealId = FCrowdyActorId::FromGuid(NetID);
	const FCrowdyActorId Invented = FCrowdyActorId::ForOwner(NetID, FGuid::NewGuid());
	const int32 Slot = Fixture.AddTrackedActor(NetID);

	const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Heard = MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
	Heard->UUID = RealId;
	TArray<FCrowdyActorUpdate> Appeared;
	FCrowdyActorUpdate& First = Appeared.AddDefaulted_GetRef();
	First.UUID = NetID;
	First.Message = Heard;
	Fixture.Tracker->RecordAppearancesForTest(Appeared);

	const auto Deliver = [&Fixture, &NetID](const FCrowdyActorId& Id)
	{
		const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message = MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
		Message->GUID = NetID;
		Message->UUID = Id;
		const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe> Shared = Message;
		const FCrowdyDelivery Delivery(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION, Message->GetPayloadKey(),
			Message->GetPayload(), Message->GetPayloadBytes(), Shared);
		Fixture.Tracker->HandleActorUpdateDeliveryForTest(Delivery);
	};

	Deliver(Invented);
	TestEqual(TEXT("its updates are dropped"), Fixture.Tracker->GetGatheredForTest().Num(), 0);
	Deliver(RealId);
	TestEqual(TEXT("and the real sender's are not"), Fixture.Tracker->GetGatheredForTest().Num(), 1);

	const TUniquePtr<FCrowdyActorTestDeliveryScope> InventedLeft = CrowdyActorTestDeparture(Invented, ECrowdyActorLeftReason::SessionReleased);
	const TUniquePtr<FCrowdyActorTestDeliveryScope> RealLeft = CrowdyActorTestDeparture(RealId, ECrowdyActorLeftReason::SessionReleased);
	if (!TestTrue(TEXT("departure deliveries were built"), InventedLeft.IsValid() && RealLeft.IsValid()))
	{
		return false;
	}

	Fixture.Tracker->HandleActorLeftDelivery(*InventedLeft->Delivery);
	TestEqual(TEXT("its departure releases nothing"), Fixture.Backend->DeactivatedSlots.Num(), 0);

	// A client that really missed a move recovers the same way: the old id leaves, and the new one is heard fresh.
	Fixture.Tracker->HandleActorLeftDelivery(*RealLeft->Delivery);
	TestTrue(TEXT("the real id leaving releases the entity"), Fixture.Backend->DeactivatedSlots.Contains(Slot));
	Fixture.Tracker->MarkActorTrackedForTest(NetID);
	Deliver(FCrowdyActorId::ForOwner(NetID, FGuid::NewGuid()));
	TestEqual(TEXT("after which an owner id is heard again"), Fixture.Tracker->GetGatheredForTest().Num(), 2);

	Fixture.Tracker->ReleaseQueuesForTest();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
