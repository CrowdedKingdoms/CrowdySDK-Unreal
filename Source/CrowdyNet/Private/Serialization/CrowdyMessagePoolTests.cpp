// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Misc/AutomationTest.h"
#include "Serialization/CrowdyMessagePool.h"

// The pool exists to stop allocating a message object per received message. What makes it safe rather than
// merely cheap is that it only ever hands back a slot nothing else still holds, because the receive path
// retains the message it decoded instead of copying the payload out of it. These cases are that property.
namespace
{
	constexpr EAutomationTestFlags CrowdyMessagePoolTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	using FActorUpdatePool = TCrowdyMessagePool<FActorUpdateNotificationMessage>;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessagePoolReusesAReleasedSlotTest,
	"CrowdySDK.Wire.MessagePoolReusesAReleasedSlot",
	CrowdyMessagePoolTestFlags)

bool FCrowdyMessagePoolReusesAReleasedSlotTest::RunTest(const FString&)
{
	FActorUpdatePool Pool;

	const FActorUpdateNotificationMessage* First = nullptr;
	{
		const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message = Pool.Acquire();
		First = &Message.Get();
	}

	const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Second = Pool.Acquire();

	// Identity is the assertion. A pool that quietly allocated every time would pass any test that only
	// looked at the message's contents, so the reuse count is read back beside it as the sentinel.
	TestTrue(TEXT("the released slot is handed out again"), &Second.Get() == First);
	TestEqual(TEXT("and the pool says it reused rather than allocated"), Pool.GetReuseCount(), (int64)1);
	TestEqual(TEXT("one message in flight needs one slot"), Pool.NumSlots(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessagePoolNeverRecyclesARetainedMessageTest,
	"CrowdySDK.Wire.MessagePoolNeverRecyclesARetainedMessage",
	CrowdyMessagePoolTestFlags)

bool FCrowdyMessagePoolNeverRecyclesARetainedMessageTest::RunTest(const FString&)
{
	// The receive path keeps the message past its delivery, so the pool must not be able to hand the same
	// object to a second decode while the first holder is still reading it. Everything acquired here is
	// held for the whole case, which is what a frame of retained updates looks like.
	FActorUpdatePool Pool;

	TArray<TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>> Held;
	TSet<const FActorUpdateNotificationMessage*> Seen;

	for (int32 Index = 0; Index < 64; ++Index)
	{
		TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message = Pool.Acquire();

		bool bAlreadySeen = false;
		Seen.Add(&Message.Get(), &bAlreadySeen);

		TestFalse(TEXT("a message still held is never handed out a second time"), bAlreadySeen);
		Held.Add(MoveTemp(Message));
	}

	TestEqual(TEXT("64 held at once needs 64 distinct messages"), Seen.Num(), 64);
	TestEqual(TEXT("and the pool never reused while they were all held"), Pool.GetReuseCount(), (int64)0);

	// Release them all and the pool goes back to serving from its slots rather than growing further.
	Held.Reset();

	const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> AfterRelease = Pool.Acquire();
	TestTrue(TEXT("a released message is reused once nothing holds it"), Pool.GetReuseCount() > 0);
	TestTrue(TEXT("and it is one of the slots already grown"), Seen.Contains(&AfterRelease.Get()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessagePoolResetsBeforeHandingBackTest,
	"CrowdySDK.Wire.MessagePoolResetsBeforeHandingBack",
	CrowdyMessagePoolTestFlags)

bool FCrowdyMessagePoolResetsBeforeHandingBackTest::RunTest(const FString&)
{
	// A reused message must not carry a previous one's value into the next decode. A decoder that writes a
	// field only under some condition would otherwise read the last message's value as this message's, and
	// nothing about that looks like a failure.
	FActorUpdatePool Pool;

	const FActorUpdateNotificationMessage Fresh;
	const FGuid Marker = FGuid::NewGuid();
	const TArray<uint8> Borrowed = { 1, 2, 3, 4 };

	{
		const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message = Pool.Acquire();
		Message->GUID = Marker;
		Message->Timestamp = 1234567;
		Message->StateSize = 99;
		Message->StateView = Borrowed;
		Message->PayloadTypeID = 77;
	}

	const TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Reused = Pool.Acquire();

	TestEqual(TEXT("the slot really was reused"), Pool.GetReuseCount(), (int64)1);
	TestNotEqual(TEXT("the previous message's id does not survive"), Reused->GUID, Marker);
	TestEqual(TEXT("nor its envelope"), Reused->Timestamp, Fresh.Timestamp);
	TestEqual(TEXT("nor its declared state size"), Reused->StateSize, Fresh.StateSize);
	TestEqual(TEXT("nor its payload type"), Reused->PayloadTypeID, Fresh.PayloadTypeID);
	TestFalse(TEXT("nor its decoded payload"), Reused->State.IsValid());

	// The view especially: it points into the frame the previous message arrived in, and that frame is
	// gone. A reused message still holding it would read freed bytes rather than fail.
	TestEqual(TEXT("and the view into the previous frame is gone"), Reused->StateView.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessagePoolStopsGrowingAtItsCeilingTest,
	"CrowdySDK.Wire.MessagePoolStopsGrowingAtItsCeiling",
	CrowdyMessagePoolTestFlags)

bool FCrowdyMessagePoolStopsGrowingAtItsCeilingTest::RunTest(const FString&)
{
	// Past the ceiling the pool still answers, by allocating. A ceiling that refused instead would drop a
	// received message, which is a far worse trade than an allocation.
	FActorUpdatePool Pool(2);

	TArray<TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>> Held;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Held.Add(Pool.Acquire());
	}

	TestEqual(TEXT("every acquire was answered"), Held.Num(), 5);
	TestEqual(TEXT("but the pool stopped growing at its ceiling"), Pool.NumSlots(), 2);
	TestEqual(TEXT("and counted what it had to allocate past it"), Pool.GetOverflowCount(), (int64)3);
	return true;
}

#endif
