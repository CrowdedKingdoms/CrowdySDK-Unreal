#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Data/CrowdyEntityTypes.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/Subsystems/CrowdyActorDepartureTestSupport.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// A Leader entity's writer can go at any moment, and the updates stop the same way whether the Leader went or this
// client only walked out of range. So the body waits long enough for a Leader change to finish, and no longer.
namespace
{
	constexpr EAutomationTestFlags CrowdyLeaderHoldTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	struct FCrowdyLeaderHoldFixture
	{
		const FGuid Local = FGuid::NewGuid();
		const FGuid Leader = FGuid::NewGuid();
		UWorld* World = nullptr;
		TStrongObjectPtr<UCrowdyActorTracker> Tracker;
		TStrongObjectPtr<UCrowdyActorManager> Manager;
		TStrongObjectPtr<UCrowdyRecordingBackend> Backend;
		TStrongObjectPtr<UCrowdyEntitySubsystem> Entities;

		FCrowdyLeaderHoldFixture()
			: Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()))
			, Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()))
			, Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()))
			, Entities(MakeEntitySubsystem(Local))
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);

			Entities->SetGameSessionForTest(MakeGameSession(Leader));
			Manager->SetBackend(Backend.Get());
			Manager->SetEntitySubsystemForTest(Entities.Get());
			Manager->BindToTrackerForTest(Tracker.Get());
			Manager->BindToEntitySubsystemForTest(Entities.Get());
			Manager->RegisterStateClass(FCrowdyActorState::StaticStruct(), ACrowdyLeaderMovingTestActor::StaticClass());
			Tracker->SetupQueuesForTest();
		}

		~FCrowdyLeaderHoldFixture()
		{
			Tracker->ReleaseQueuesForTest();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		// An entity this client draws, led by someone else, and heard so far. A placed one resolved its own identity.
		FGuid AddEntity(const bool bLeaderOwned = true, const bool bPlaced = false) const
		{
			ACrowdyLeaderMovingTestActor* Actor = World->SpawnActor<ACrowdyLeaderMovingTestActor>();
			Actor->Entity->SetCollaboratorsForTest(Entities.Get(), nullptr);

			FCrowdyEntityRecord Record;
			Record.NetID = FGuid::NewGuid();
			Record.OwnerID = Leader;
			Record.Role = ECrowdyRole::RemoteProxy;
			Record.bLeaderOwned = bLeaderOwned;
			Record.Participant = Actor;
			Entities->RegisterEntity(Record);
			if (!bPlaced)
			{
				Actor->Entity->AssignPooledIdentity(Record.NetID, Leader, ECrowdyRole::RemoteProxy, 0);
			}
			Hear(Record.NetID);
			return Record.NetID;
		}

		// The first update after the tracker lost an entity reaches the manager as an appearance.
		void Hear(const FGuid& NetID) const
		{
			Tracker->MarkActorTrackedForTest(NetID);
			Tracker->OnRemoteEntityAppeared.Broadcast(NetID, FInstancedStruct::Make(FCrowdyActorState()), 1);
		}

		void Depart(const FGuid& NetID) const
		{
			const TUniquePtr<FCrowdyActorTestDeliveryScope> Left =
				CrowdyActorTestDeparture(FCrowdyActorId::FromGuid(NetID), ECrowdyActorLeftReason::SessionReleased);
			Tracker->HandleActorLeftDelivery(*Left->Delivery);
		}

		void PassTheWait() const
		{
			Manager->TickHeldEntitiesForTest(FPlatformTime::Seconds() + UCrowdyActorManager::GetHoldSecondsForTest() + 1.0);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderBodyHoldsTest,
	"CrowdySDK.LeaderHold.ABodyWaitsWhenItsWriterGoesQuiet", CrowdyLeaderHoldTestFlags)
bool FCrowdyLeaderBodyHoldsTest::RunTest(const FString& Parameters)
{
	const FCrowdyLeaderHoldFixture Fixture;

	const FGuid Led = Fixture.AddEntity();
	Fixture.Depart(Led);
	TestTrue(TEXT("a Leader entity is held"), Fixture.Manager->IsEntityHeldForTest(Led));
	TestTrue(TEXT("and keeps its slot"), Fixture.Manager->HasSlotForTest(Led));
	TestEqual(TEXT("its body is not released"), Fixture.Backend->DeactivatedSlots.Num(), 0);

	Fixture.Hear(Led);
	TestFalse(TEXT("being heard again ends the hold"), Fixture.Manager->IsEntityHeldForTest(Led));

	Fixture.PassTheWait();
	TestTrue(TEXT("so the wait running out releases nothing"), Fixture.Manager->HasSlotForTest(Led));

	const FGuid Players = Fixture.AddEntity(/*bLeaderOwned=*/false);
	Fixture.Depart(Players);
	TestFalse(TEXT("any other entity is not held"), Fixture.Manager->IsEntityHeldForTest(Players));
	TestEqual(TEXT("its body goes with its writer"), Fixture.Backend->DeactivatedSlots.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyLeaderBodyWaitEndsTest,
	"CrowdySDK.LeaderHold.ASpawnedBodyIsReleasedOnceTheWaitRunsOut", CrowdyLeaderHoldTestFlags)
bool FCrowdyLeaderBodyWaitEndsTest::RunTest(const FString& Parameters)
{
	const FCrowdyLeaderHoldFixture Fixture;

	const FGuid Spawned = Fixture.AddEntity();
	const FGuid Placed = Fixture.AddEntity(/*bLeaderOwned=*/true, /*bPlaced=*/true);
	Fixture.Depart(Spawned);
	Fixture.Depart(Placed);

	Fixture.Manager->TickHeldEntitiesForTest(FPlatformTime::Seconds());
	TestTrue(TEXT("a Leader change is given time to finish"), Fixture.Manager->IsEntityHeldForTest(Spawned));

	Fixture.PassTheWait();
	TestFalse(TEXT("a spawned body is then given up"), Fixture.Manager->HasSlotForTest(Spawned));
	TestFalse(TEXT("and is no longer held"), Fixture.Manager->IsEntityHeldForTest(Spawned));
	TestEqual(TEXT("through the backend"), Fixture.Backend->DeactivatedSlots.Num(), 1);
	TestTrue(TEXT("a placed one has nowhere to come back from"), Fixture.Manager->IsEntityHeldForTest(Placed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyHoldEndsWithTheEntityTest,
	"CrowdySDK.LeaderHold.ADestroyedEntityGivesItsSlotBack", CrowdyLeaderHoldTestFlags)
bool FCrowdyHoldEndsWithTheEntityTest::RunTest(const FString& Parameters)
{
	const FCrowdyLeaderHoldFixture Fixture;

	const FGuid NetID = Fixture.AddEntity();
	Fixture.Depart(NetID);

	Fixture.Entities->UnregisterEntity(NetID);
	TestFalse(TEXT("no hold is left behind"), Fixture.Manager->IsEntityHeldForTest(NetID));
	TestFalse(TEXT("and no slot"), Fixture.Manager->HasSlotForTest(NetID));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTakingOverLetsGoOfTheSlotTest,
	"CrowdySDK.LeaderHold.TakingOverSetsTheSlotUpAtOnce", CrowdyLeaderHoldTestFlags)
bool FCrowdyTakingOverLetsGoOfTheSlotTest::RunTest(const FString& Parameters)
{
	const FCrowdyLeaderHoldFixture Fixture;

	const FGuid NetID = Fixture.AddEntity();
	Fixture.Depart(NetID);
	const int32 ActivationsBefore = Fixture.Backend->ActivatedSlots.Num();

	Fixture.Entities->ReassignOwnership(NetID, Fixture.Local, FGuid());
	TestFalse(TEXT("the hold ends with this client writing"), Fixture.Manager->IsEntityHeldForTest(NetID));
	TestEqual(TEXT("the backend is told with no update to prompt it"), Fixture.Backend->ActivatedSlots.Num(), ActivationsBefore + 1);

	Fixture.Hear(NetID);
	Fixture.Depart(NetID);
	TestFalse(TEXT("a client never holds what it writes"), Fixture.Manager->IsEntityHeldForTest(NetID));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTakingOverOnRowsTest,
	"CrowdySDK.LeaderHold.TakingOverLeavesACrowdRowForItsNextUpdate", CrowdyLeaderHoldTestFlags)
bool FCrowdyTakingOverOnRowsTest::RunTest(const FString& Parameters)
{
	const FCrowdyLeaderHoldFixture Fixture;
	UCrowdyCrowdRowRecordingBackend* Rows = NewObject<UCrowdyCrowdRowRecordingBackend>(GetTransientPackage());
	Fixture.Manager->SetBackend(Rows);

	const FGuid NetID = Fixture.AddEntity();
	const int32 ActivationsBefore = Rows->ActivatedSlots.Num();

	Fixture.Entities->ReassignOwnership(NetID, Fixture.Local, FGuid());
	TestEqual(TEXT("a row needs the state only an update carries"), Rows->ActivatedSlots.Num(), ActivationsBefore);
	return true;
}

#endif
