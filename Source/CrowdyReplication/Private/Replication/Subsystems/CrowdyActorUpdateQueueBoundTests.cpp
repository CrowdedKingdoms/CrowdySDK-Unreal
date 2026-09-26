// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// A remote sender decides how fast actor updates arrive, so every queue they wait in has a ceiling. Past it an
// update is dropped and counted rather than held, and the queue takes updates again as soon as it drains.
namespace
{
	constexpr EAutomationTestFlags CrowdyActorUpdateQueueBoundTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyActorUpdate MakeUpdate(const FGuid& UUID)
	{
		FCrowdyActorUpdate Update;
		Update.UUID = UUID;
		Update.ServerTimestamp = 1000;
		return Update;
	}

	// Sets the tracker's queue ceiling for one case and puts the project's value back afterwards.
	struct FScopedQueuedUpdateCeiling
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.replication.tracker.maxqueuedupdates"));
		int32 Previous = Variable ? Variable->GetInt() : 0;

		explicit FScopedQueuedUpdateCeiling(const int32 Ceiling)
		{
			if (Variable)
			{
				Variable->Set(Ceiling, ECVF_SetByCode);
			}
		}

		~FScopedQueuedUpdateCeiling()
		{
			if (Variable)
			{
				Variable->Set(Previous, ECVF_SetByCode);
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyTrackerQueueDropsPastItsCeilingTest,
	"CrowdySDK.Replication.TrackerQueueDropsPastItsCeiling",
	CrowdyActorUpdateQueueBoundTestFlags)

bool FCrowdyTrackerQueueDropsPastItsCeilingTest::RunTest(const FString&)
{
	const FScopedQueuedUpdateCeiling Ceiling(4);
	if (!TestNotNull(TEXT("the ceiling is a console variable"), Ceiling.Variable))
	{
		return false;
	}

	// No worker pool stands behind this tracker, which is exactly the state in which nothing drains the queues.
	AddExpectedMessagePlain(TEXT("worker pool is not accepting work"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("dropped in total"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	Tracker->SetupQueuesForTest();

	for (int32 Index = 0; Index < 6; ++Index)
	{
		Tracker->EnqueueUpdateForTest(MakeUpdate(FGuid::NewGuid()));
	}

	TestEqual(TEXT("the queues hold no more than the ceiling"), Tracker->NumQueuedUpdatesForTest(), 4);
	TestEqual(TEXT("and count what they refused"), Tracker->NumQueuedUpdatesDroppedForTest(), 2);

	// Consumers taking updates out is what gives the ceiling back.
	Tracker->ProcessAllShardsForTest();
	TestEqual(TEXT("a drained queue counts nothing waiting"), Tracker->NumQueuedUpdatesForTest(), 0);

	Tracker->EnqueueUpdateForTest(MakeUpdate(FGuid::NewGuid()));
	TestEqual(TEXT("and takes updates again"), Tracker->NumQueuedUpdatesForTest(), 1);
	TestEqual(TEXT("without counting another drop"), Tracker->NumQueuedUpdatesDroppedForTest(), 2);

	Tracker->ReleaseQueuesForTest();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorManagerOffThreadQueueDropsPastItsCeilingTest,
	"CrowdySDK.Replication.ActorManagerOffThreadQueueDropsPastItsCeiling",
	CrowdyActorUpdateQueueBoundTestFlags)

bool FCrowdyActorManagerOffThreadQueueDropsPastItsCeilingTest::RunTest(const FString&)
{
	AddExpectedMessagePlain(TEXT("already waiting for the next tick"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRecordingBackend> Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));
	Manager->SetBackend(Backend.Get());

	const FGuid UUID = FGuid::NewGuid();
	Manager->AllocateSlotForTest(UUID);

	const int32 Ceiling = UCrowdyActorManager::GetMaxOffThreadUpdatesForTest();
	for (int32 Index = 0; Index < Ceiling + 3; ++Index)
	{
		Manager->EnqueueOffThreadForTest(MakeUpdate(UUID));
	}

	TestEqual(TEXT("everything past the ceiling is counted as dropped"), Manager->NumOffThreadUpdatesDroppedForTest(), 3);

	Manager->ApplyPendingUpdatesForTest();
	TestEqual(TEXT("the ceiling's worth reaches the backend"), Backend->ExtractedSlots.Num(), Ceiling);

	// The drain gave the ceiling back, so the next update is queued rather than dropped.
	Manager->EnqueueOffThreadForTest(MakeUpdate(UUID));
	Manager->ApplyPendingUpdatesForTest();
	TestEqual(TEXT("a drained queue takes updates again"), Backend->ExtractedSlots.Num(), Ceiling + 1);
	TestEqual(TEXT("without counting another drop"), Manager->NumOffThreadUpdatesDroppedForTest(), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorManagerOffThreadQueueClearsWithoutABackendTest,
	"CrowdySDK.Replication.ActorManagerOffThreadQueueClearsWithoutABackend",
	CrowdyActorUpdateQueueBoundTestFlags)

bool FCrowdyActorManagerOffThreadQueueClearsWithoutABackendTest::RunTest(const FString&)
{
	// Updates queued while no backend exists are stale by the time one arrives, so they go rather than wait.
	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const FGuid UUID = FGuid::NewGuid();
	Manager->AllocateSlotForTest(UUID);

	Manager->EnqueueOffThreadForTest(MakeUpdate(UUID));
	Manager->EnqueueOffThreadForTest(MakeUpdate(UUID));
	Manager->ApplyPendingUpdatesForTest();

	const TStrongObjectPtr<UCrowdyRecordingBackend> Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));
	Manager->SetBackend(Backend.Get());
	Manager->ApplyPendingUpdatesForTest();
	TestEqual(TEXT("a backend that arrives later is handed none of them"), Backend->ExtractedSlots.Num(), 0);

	// And the ceiling they held is given back.
	for (int32 Index = 0; Index < UCrowdyActorManager::GetMaxOffThreadUpdatesForTest(); ++Index)
	{
		Manager->EnqueueOffThreadForTest(MakeUpdate(UUID));
	}
	TestEqual(TEXT("the whole ceiling is available again"), Manager->NumOffThreadUpdatesDroppedForTest(), 0);
	return true;
}

#endif
