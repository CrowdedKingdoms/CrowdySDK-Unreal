#if WITH_DEV_AUTOMATION_TESTS

#include "CoreGlobals.h"
#include "CrowdyReplicationLog.h"
#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Data/CrowdyTransformRepPolicy.h"
#include "Messages/Actor/FCrowdyKitActorState.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Replication/Components/CrowdyKitActorComponent.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyKitActorClasses.h"
#include "Replication/Subsystems/CrowdyKitActorSubsystem.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyKitActorTestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyKitActorState CrowdyKitActorTestMob(const int64 Id, const int64 Hp)
	{
		FCrowdyKitActorState Mob;
		Mob.Kind = TEXT("mob");
		Mob.Id = Id;
		Mob.Location = FVector(100.0, 200.0, 0.0);
		Mob.bHasHp = true;
		Mob.Hp = Hp;
		Mob.MaxHp = 100;
		Mob.Mode = TEXT("idle");
		return Mob;
	}

	FCrowdyActorUpdate CrowdyKitActorTestUpdate(const FGuid& Key, const FInstancedStruct& State)
	{
		FCrowdyActorUpdate Update;
		Update.UUID = Key;
		Update.State = State;
		return Update;
	}

	// LogCrowdyReplication warnings, read as the lines themselves.
	class FCrowdyKitActorWarningCapture : public FOutputDevice
	{
	public:
		FCrowdyKitActorWarningCapture() { GLog->AddOutputDevice(this); }
		virtual ~FCrowdyKitActorWarningCapture() override { GLog->RemoveOutputDevice(this); }

		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Verbosity == ELogVerbosity::Warning && Category == LogCrowdyReplication.GetCategoryName())
			{
				FScopeLock Lock(&Mutex);
				Lines.Add(Message);
			}
		}

		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		int32 Count(const TCHAR* Needle)
		{
			GLog->FlushThreadedLogs();
			FScopeLock Lock(&Mutex);
			return Lines.FilterByPredicate([Needle](const FString& Line) { return Line.Contains(Needle); }).Num();
		}

	private:
		FCriticalSection Mutex;
		TArray<FString> Lines;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitActorClassByKindTest,
	"CrowdySDK.ActorManager.KitActorIsDrawnAsItsKindsClass", CrowdyKitActorTestFlags)
bool FCrowdyKitActorClassByKindTest::RunTest(const FString& Parameters)
{
	CrowdyKitActorClasses::Reset();
	CrowdyKitActorClasses::Set(TEXT("mob"), TSoftClassPtr<AActor>(ACrowdyPoolCollisionProbeActor::StaticClass()));
	ON_SCOPE_EXIT { CrowdyKitActorClasses::Reset(); };

	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRecordingBackend> Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));
	Manager->SetBackend(Backend.Get());
	Manager->BindToTrackerForTest(Tracker.Get());

	Tracker->OnRemoteEntityAppeared.Broadcast(FGuid::NewGuid(), FInstancedStruct::Make(CrowdyKitActorTestMob(3, 100)), 1);
	if (!TestEqual(TEXT("a mob is drawn"), Backend->ActivatedClasses.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("as the class its kind names"), Backend->ActivatedClasses[0] == ACrowdyPoolCollisionProbeActor::StaticClass());
	TestNotNull(TEXT("with its kit state"), Backend->ActivatedStates[0].GetPtr<FCrowdyKitActorState>());

	FCrowdyKitActorWarningCapture Capture;
	FCrowdyKitActorState Dragon = CrowdyKitActorTestMob(4, 100);
	Dragon.Kind = TEXT("dragon");
	Tracker->OnRemoteEntityAppeared.Broadcast(FGuid::NewGuid(), FInstancedStruct::Make(Dragon), 2);
	Tracker->OnRemoteEntityAppeared.Broadcast(FGuid::NewGuid(), FInstancedStruct::Make(Dragon), 3);
	TestEqual(TEXT("a kind no starter gives a class is not drawn"), Backend->ActivatedClasses.Num(), 1);
	TestEqual(TEXT("which is said once for the kind"), Capture.Count(TEXT("shows a dragon")), 1);

	const int32 Bound = UCrowdyActorManager::GetPendingActivationEvictTicksForTest();
	for (int32 Tick = 0; Tick < Bound; ++Tick)
	{
		Manager->TickPendingActivationsForTest();
	}
	TestEqual(TEXT("and it is not parked waiting for a spawn event that never comes"), Capture.Count(TEXT("has been released")), 0);

	CrowdyKitActorClasses::Set(TEXT("mob"), TSoftClassPtr<AActor>(AActor::StaticClass()));
	TestEqual(TEXT("two starters drawing one kind is said"), Capture.Count(TEXT("two starters in Game Kit Actor Starters draw a mob")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitActorTimeoutTest,
	"CrowdySDK.ActorTracker.KitActorsTimeOutOnTheirOwnBound", CrowdyKitActorTestFlags)
bool FCrowdyKitActorTimeoutTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	// What Initialize does first: the sets a time-out clears.
	Tracker->Configure(64, 8, 0.002f, 12.0f);
	Tracker->SetKitActorTimeoutForTest(1.0f);
	const FGuid SilentMob = FGuid::NewGuid();
	const FGuid LiveMob = FGuid::NewGuid();
	const FGuid Player = FGuid::NewGuid();
	Tracker->NoteHeardForTest({ CrowdyKitActorTestUpdate(SilentMob, FInstancedStruct::Make(CrowdyKitActorTestMob(1, 100))),
		CrowdyKitActorTestUpdate(Player, FInstancedStruct()) }, 2.0);
	Tracker->NoteHeardForTest({ CrowdyKitActorTestUpdate(LiveMob, FInstancedStruct::Make(CrowdyKitActorTestMob(2, 100))) }, 0.2);
	TestTrue(TEXT("a kit actor is heard as one"), Tracker->IsHeardAsKitActorForTest(SilentMob));
	TestFalse(TEXT("and any other actor is not"), Tracker->IsHeardAsKitActorForTest(Player));

	Tracker->CheckKitTimeoutsForTest();
	TestFalse(TEXT("a kit actor silent past its bound leaves"), Tracker->IsHeardForTest(SilentMob));
	TestTrue(TEXT("one heard within it stays"), Tracker->IsHeardForTest(LiveMob));
	TestTrue(TEXT("and another actor silent as long is left to its own bound"), Tracker->IsHeardForTest(Player));

	Tracker->CheckTimeoutsForTest();
	TestTrue(TEXT("which is longer"), Tracker->IsHeardForTest(Player));

	// A frame that ran long: the mob's update arrived during it, so it is heard before anything is swept.
	Tracker->MarkActorTrackedForTest(LiveMob);
	Tracker->NoteHeardForTest({ CrowdyKitActorTestUpdate(LiveMob, FInstancedStruct::Make(CrowdyKitActorTestMob(2, 100))) }, 2.0);
	Tracker->GatherForTest({ CrowdyKitActorTestUpdate(LiveMob, FInstancedStruct::Make(CrowdyKitActorTestMob(2, 90))) });
	Tracker->Tick(2.0f);
	TestTrue(TEXT("a long frame does not time out a kit actor heard during it"), Tracker->IsTrackedForTest(LiveMob));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitActorForgedUpdateTest,
	"CrowdySDK.ActorTracker.KitUpdateForAnSdkActorIsDropped", CrowdyKitActorTestFlags)
bool FCrowdyKitActorForgedUpdateTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	Tracker->Configure(64, 8, 0.002f, 12.0f);
	const FGuid Player = FGuid::NewGuid();
	const FGuid Mob = FGuid::NewGuid();
	const FGuid Arriving = FGuid::NewGuid();
	FCrowdyActorUpdate PlayerAppeared = CrowdyKitActorTestUpdate(Player, FInstancedStruct());
	PlayerAppeared.PayloadTypeID = 7;
	Tracker->RecordAppearancesForTest({ PlayerAppeared, CrowdyKitActorTestUpdate(Mob, FInstancedStruct::Make(CrowdyKitActorTestMob(1, 100))) });

	const FInstancedStruct Kit = FInstancedStruct::Make(CrowdyKitActorTestMob(9, 1));
	Tracker->GatherForTest({ CrowdyKitActorTestUpdate(Player, Kit), CrowdyKitActorTestUpdate(Mob, Kit), CrowdyKitActorTestUpdate(Arriving, Kit), PlayerAppeared });
	Tracker->DropForgedKitUpdatesForTest();
	const TArray<FCrowdyActorUpdate>& Kept = Tracker->GetGatheredForTest();
	TestEqual(TEXT("a kit update claiming a player is dropped, and nothing else"), Kept.Num(), 3);
	TestFalse(TEXT("the one dropped is the player's kit update"), Kept.ContainsByPredicate([&Player](const FCrowdyActorUpdate& Update)
	{
		return Update.UUID == Player && UCrowdyActorTracker::IsKitActorUpdate(Update);
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitActorTransformTest,
	"CrowdySDK.ActorManager.KitActorMovesWithTheActorPool", CrowdyKitActorTestFlags)
bool FCrowdyKitActorTransformTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyTransformRepPolicy> Policy(NewObject<UCrowdyTransformRepPolicy>(GetTransientPackage()));
	TestTrue(TEXT("the actor pool's movement takes a kit actor's place"),
		Policy->ExtractFields(FInstancedStruct::Make(CrowdyKitActorTestMob(3, 100)), 1000, 0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitActorFactsTest,
	"CrowdySDK.ActorManager.KitActorFactsAnnounceWhatChanged", CrowdyKitActorTestFlags)
bool FCrowdyKitActorFactsTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyKitActorComponent> Component(NewObject<UCrowdyKitActorComponent>(GetTransientPackage()));
	const FGuid Wolf = FGuid::NewGuid();
	FCrowdyKitActorState Mob = CrowdyKitActorTestMob(3, 100);
	TestTrue(TEXT("drawing a kit actor announces it"), Component->ApplyKitActor(Wolf, Mob));
	Mob.Location += FVector(50.0, 0.0, 0.0);
	TestFalse(TEXT("moving it does not"), Component->ApplyKitActor(Wolf, Mob));
	TestEqual(TEXT("though the component holds where it is now"), Component->KitActor.Location, Mob.Location);
	Mob.Hp = 90;
	TestTrue(TEXT("a hit does"), Component->ApplyKitActor(Wolf, Mob));
	Mob.Mode = TEXT("chase");
	TestTrue(TEXT("and so does its mode"), Component->ApplyKitActor(Wolf, Mob));
	TestTrue(TEXT("and drawing another actor with the same facts"), Component->ApplyKitActor(FGuid::NewGuid(), Mob));
	const FGuid Again = Component->GetKitKey();
	TestTrue(TEXT("and a pooled actor drawing the same one again"), Component->ApplyKitActor(Again, Mob, true));

	const TStrongObjectPtr<UCrowdyKitActorSubsystem> Seen(NewObject<UCrowdyKitActorSubsystem>(GetTransientPackage()));
	FCrowdyKitActorState Npc = CrowdyKitActorTestMob(5, 0);
	Npc.Kind = TEXT("npc");
	Npc.bHasHp = false;
	Seen->TakeUpdates({ CrowdyKitActorTestUpdate(Wolf, FInstancedStruct::Make(CrowdyKitActorTestMob(3, 80))),
		CrowdyKitActorTestUpdate(FGuid::NewGuid(), FInstancedStruct::Make(Npc)) });
	TestEqual(TEXT("every kit actor seen is listed"), Seen->GetKitActors(NAME_None).Num(), 2);
	const TArray<FCrowdyKitActorState> Mobs = Seen->GetKitActors(TEXT("mob"));
	TestTrue(TEXT("and by kind, as its hub last showed it"), Mobs.Num() == 1 && Mobs[0].Hp == 80);
	Seen->TakeUpdates({ CrowdyKitActorTestUpdate(Wolf, FInstancedStruct::Make(CrowdyKitActorTestMob(3, 70))) });
	TestTrue(TEXT("a later update replaces it"), Seen->FindKitActor(Wolf) && Seen->FindKitActor(Wolf)->Hp == 70);
	Seen->Forget(Wolf);
	TestNull(TEXT("and one that left is no longer listed"), Seen->FindKitActor(Wolf));
	return true;
}

#endif
