// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Data/CrowdyRenderingBackendTestTypes.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Misc/AutomationTest.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/Subsystems/CrowdyActorManager.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// An actor update carries its state one of two ways: the internal chain retains the message it was decoded
// from, and anything that crossed a dynamic delegate carries the copied property instead. Which one is set
// is not observable from the outside, so ResolveState is the only door, and these cases are what say it
// opens on the right one. The third is the load-bearing one: retaining the message keeps its DECODED state
// and deliberately does not keep the frame's octets, so the state has to survive the buffer it arrived in.
namespace
{
	constexpr EAutomationTestFlags CrowdyActorUpdateStateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FInstancedStruct MakeState(const double X)
	{
		FCrowdyActorState Value;
		Value.Location = FVector(X, 0.0, 0.0);
		return FInstancedStruct::Make(Value);
	}

	double ReadX(const FInstancedStruct& State)
	{
		const FCrowdyActorState* Typed = State.GetPtr<FCrowdyActorState>();
		return Typed ? Typed->Location.X : -1.0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorUpdateReadsTheRetainedMessageTest,
	"CrowdySDK.Replication.ActorUpdateReadsTheRetainedMessage",
	CrowdyActorUpdateStateTestFlags)

bool FCrowdyActorUpdateReadsTheRetainedMessageTest::RunTest(const FString&)
{
	const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message =
		MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
	Message->State = MakeState(42.0);

	FCrowdyActorUpdate Update;
	Update.Message = Message;

	// The copied property is left EMPTY on purpose. That is what the internal chain produces now, and a
	// reader that went to it rather than to the message would hand the backend an unset struct.
	TestFalse(TEXT("the internal path leaves the reflected property unset"), Update.State.IsValid());
	TestTrue(TEXT("the retained message is what carries the state"), Update.ResolveState().IsValid());
	TestEqual(TEXT("and the value read back is the sender's"), ReadX(Update.ResolveState()), 42.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorUpdateFallsBackToTheCopiedStateTest,
	"CrowdySDK.Replication.ActorUpdateFallsBackToTheCopiedState",
	CrowdyActorUpdateStateTestFlags)

bool FCrowdyActorUpdateFallsBackToTheCopiedStateTest::RunTest(const FString&)
{
	// What an update looks like after it has crossed the game-thread delegate: the message cannot be
	// marshalled property by property, so it is released and the state is materialised into the property.
	FCrowdyActorUpdate Update;
	Update.State = MakeState(7.0);

	TestFalse(TEXT("no message is retained on the Blueprint path"), Update.Message.IsValid());
	TestEqual(TEXT("the copied property is read instead"), ReadX(Update.ResolveState()), 7.0);

	// A default update has neither, and must read as an unset struct rather than dereferencing a null.
	const FCrowdyActorUpdate Empty;
	TestFalse(TEXT("an update carrying neither reads as unset"), Empty.ResolveState().IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorUpdateStateOutlivesItsFrameTest,
	"CrowdySDK.Replication.ActorUpdateStateOutlivesItsFrame",
	CrowdyActorUpdateStateTestFlags)

bool FCrowdyActorUpdateStateOutlivesItsFrameTest::RunTest(const FString&)
{
	FCrowdyActorUpdate Update;

	{
		// The message goes out of scope here exactly as the delivery does, leaving the update's retain as
		// the only thing holding it. This is the whole lifetime claim the slice rests on.
		const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message =
			MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
		Message->State = MakeState(99.0);
		Update.Message = Message;
	}

	TestTrue(TEXT("the retain keeps the message alive past the delivery"), Update.Message.IsValid());
	TestEqual(TEXT("and its decoded state is still readable"), ReadX(Update.ResolveState()), 99.0);

	// A second update retaining the same message is what a batch produces, and neither may disturb the other.
	FCrowdyActorUpdate Sibling;
	Sibling.Message = Update.Message;
	Update.Message.Reset();

	TestTrue(TEXT("releasing one holder does not free the message"), Sibling.Message.IsValid());
	TestEqual(TEXT("the survivor still reads the same state"), ReadX(Sibling.ResolveState()), 99.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorUpdateDrainReadsTheRetainedMessageTest,
	"CrowdySDK.Replication.ActorUpdateDrainReadsTheRetainedMessage",
	CrowdyActorUpdateStateTestFlags)

bool FCrowdyActorUpdateDrainReadsTheRetainedMessageTest::RunTest(const FString&)
{
	// Through the real queue and the real drain rather than through ExtractUpdateForTest, which hands the
	// backend a struct directly and therefore cannot see which of the two carriers the drain chose.
	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRecordingBackend> Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));
	Manager->SetBackend(Backend.Get());

	const FGuid UUID = FGuid::NewGuid();
	Manager->AllocateSlotForTest(UUID);

	const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message =
		MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
	Message->State = MakeState(42.0);

	FCrowdyActorUpdate Update;
	Update.UUID = UUID;
	Update.ServerTimestamp = 1000;
	Update.Message = Message;

	// Left empty deliberately. This is what the receive path now produces, so a drain that read the
	// reflected property would hand the backend an unset struct and every entity would stop moving.
	TestFalse(TEXT("the update carries no copied state"), Update.State.IsValid());

	TArray<FCrowdyActorUpdate> Batch;
	Batch.Add(Update);
	Manager->DrainUpdateBatchForTest(Batch);

	TestEqual(TEXT("the drain reached the backend once"), Backend->ExtractedStates.Num(), 1);
	if (Backend->ExtractedStates.Num() != 1)
	{
		return false;
	}

	TestTrue(TEXT("the backend was handed a valid state"), Backend->ExtractedStates[0].IsValid());
	TestEqual(TEXT("and it is the retained message's, not the empty property"),
		ReadX(Backend->ExtractedStates[0]), 42.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorUpdateDrainReadsHandedOverBeforeGatheredTest,
	"CrowdySDK.Replication.ActorUpdateDrainReadsHandedOverBeforeGathered",
	CrowdyActorUpdateStateTestFlags)

bool FCrowdyActorUpdateDrainReadsHandedOverBeforeGatheredTest::RunTest(const FString&)
{
	// Updates reach the drain two ways: handed over from another thread through the queue, or appended on
	// the game thread that received them. Anything in the queue was gathered before anything appended after
	// it, so the queue has to be read first or an older position would be applied over a newer one.
	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRecordingBackend> Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));
	Manager->SetBackend(Backend.Get());

	const FGuid UUID = FGuid::NewGuid();
	Manager->AllocateSlotForTest(UUID);

	FCrowdyActorUpdate Older;
	Older.UUID = UUID;
	Older.ServerTimestamp = 1000;
	Older.State = MakeState(1.0);
	Manager->EnqueueOffThreadForTest(Older);

	FCrowdyActorUpdate Newer;
	Newer.UUID = UUID;
	Newer.ServerTimestamp = 2000;
	Newer.State = MakeState(2.0);

	TArray<FCrowdyActorUpdate> Gathered;
	Gathered.Add(Newer);
	Manager->DrainUpdateBatchForTest(Gathered);

	TestEqual(TEXT("both sources reached the backend"), Backend->ExtractedStates.Num(), 2);
	if (Backend->ExtractedStates.Num() != 2)
	{
		return false;
	}

	// The order is the assertion, not the count: reading the array first would leave the slot holding 1.0.
	TestEqual(TEXT("the handed-over update was applied first"), ReadX(Backend->ExtractedStates[0]), 1.0);
	TestEqual(TEXT("and the gathered one last, so the newest position is what stands"),
		ReadX(Backend->ExtractedStates[1]), 2.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorUpdateDrainClearsGatheredUpdatesTest,
	"CrowdySDK.Replication.ActorUpdateDrainClearsGatheredUpdates",
	CrowdyActorUpdateStateTestFlags)

bool FCrowdyActorUpdateDrainClearsGatheredUpdatesTest::RunTest(const FString&)
{
	// The gathered array is reused across frames rather than freed, so a drain that forgot to clear it
	// would re-apply the previous frame's positions every frame after, which reads as entities stuttering
	// back rather than as anything failing.
	const TStrongObjectPtr<UCrowdyActorManager> Manager(NewObject<UCrowdyActorManager>(GetTransientPackage()));
	const TStrongObjectPtr<UCrowdyRecordingBackend> Backend(NewObject<UCrowdyRecordingBackend>(GetTransientPackage()));
	Manager->SetBackend(Backend.Get());

	const FGuid UUID = FGuid::NewGuid();
	Manager->AllocateSlotForTest(UUID);

	FCrowdyActorUpdate Update;
	Update.UUID = UUID;
	Update.ServerTimestamp = 1000;
	Update.State = MakeState(5.0);

	TArray<FCrowdyActorUpdate> Batch;
	Batch.Add(Update);
	Manager->DrainUpdateBatchForTest(Batch);

	TestEqual(TEXT("the first drain applied it once"), Backend->ExtractedStates.Num(), 1);

	// A second drain with nothing new must apply nothing at all.
	Manager->DrainUpdateBatchForTest(TArray<FCrowdyActorUpdate>());

	TestEqual(TEXT("a drain with nothing new applies nothing again"), Backend->ExtractedStates.Num(), 1);
	return true;
}

#endif
