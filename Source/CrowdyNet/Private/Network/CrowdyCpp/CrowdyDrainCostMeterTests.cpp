#include "Misc/AutomationTest.h"
#include "Network/CrowdyCpp/CrowdyDrainCostMeter.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// The application context mask rather than the editor one, because the suite is run headless: an
	// editor-only test is simply absent from that run rather than failing in it.
	constexpr EAutomationTestFlags CrowdyDrainCostTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** The drain window crowdy.net.receive.maxdrainms currently ships, in seconds. */
	constexpr double ShippedDrainWindow = 0.004;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDrainCostWeighsByMessageTest,
	"CrowdySDK.CrowdyNet.DrainCostWeighsByMessage",
	CrowdyDrainCostTestFlags)

bool FCrowdyDrainCostWeighsByMessageTest::RunTest(const FString&)
{
	FCrowdyDrainCostMeter Meter;

	// Two drains of deliberately different length and cost: 2 us a message over 100, then 4 us over 300.
	Meter.Record(100, 0.0002);
	Meter.Record(300, 0.0012);

	const FCrowdyDrainCost Cost = Meter.Read(ShippedDrainWindow);

	TestEqual(TEXT("every delivered message is counted"), Cost.Messages, static_cast<int64>(400));

	// Reported alongside the cost because the cost alone cannot say whether the allowance is being reached,
	// and a run's delivered-per-second and its allowance-exhausted drain count have disagreed by a fifth.
	TestEqual(TEXT("and so is every drain that delivered one"), Cost.Drains, static_cast<int64>(2));
	TestEqual(TEXT("so the average drain size is readable"), Cost.MessagesPerDrain, 200.0, 0.0001);

	// 3.5 and not 3.0. A mean of the two drains' rates would give 3.0, which would let a handful of short
	// cheap drains outvote the long expensive ones that actually spend the budget.
	TestEqual(TEXT("the cost is weighted by messages, not by drains"), Cost.MicrosecondsPerMessage, 3.5, 0.0001);

	// 4 ms at 3.5 us each, floored: the window does not afford the message it can only part pay for.
	TestEqual(TEXT("the window affords what it pays for"), Cost.MessagesAffordedByWindow, 1142);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDrainCostLeavesOutEmptyDrainsTest,
	"CrowdySDK.CrowdyNet.DrainCostLeavesOutEmptyDrains",
	CrowdyDrainCostTestFlags)

bool FCrowdyDrainCostLeavesOutEmptyDrainsTest::RunTest(const FString&)
{
	FCrowdyDrainCostMeter Meter;

	// A poll that found nothing still costs time, and charging that time to the messages some later drain
	// delivered is what turns a per-message cost into a figure that follows the idle frame rate.
	Meter.Record(0, 0.002);
	Meter.Record(-5, 0.001);
	Meter.Record(100, 0.0002);

	const FCrowdyDrainCost Cost = Meter.Read(ShippedDrainWindow);

	TestEqual(TEXT("only the drain that delivered is counted"), Cost.Messages, static_cast<int64>(100));
	TestEqual(TEXT("and an empty drain is not a drain for this purpose"), Cost.Drains, static_cast<int64>(1));
	TestEqual(TEXT("an empty drain's time is not charged to a later drain's messages"),
		Cost.MicrosecondsPerMessage, 2.0, 0.0001);
	TestEqual(TEXT("the affordable count follows the cost that was actually paid"),
		Cost.MessagesAffordedByWindow, 2000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDrainCostAnswersNothingWithoutAReadingTest,
	"CrowdySDK.CrowdyNet.DrainCostAnswersNothingWithoutAReading",
	CrowdyDrainCostTestFlags)

bool FCrowdyDrainCostAnswersNothingWithoutAReadingTest::RunTest(const FString&)
{
	FCrowdyDrainCostMeter Meter;

	TestFalse(TEXT("an untouched meter has nothing to report"), Meter.HasReading());

	const FCrowdyDrainCost Empty = Meter.Read(ShippedDrainWindow);
	TestEqual(TEXT("no messages"), Empty.Messages, static_cast<int64>(0));
	TestEqual(TEXT("no cost"), Empty.MicrosecondsPerMessage, 0.0, 0.0001);

	// Zero rather than a large number. A caller that read an affordable count off an unmeasured meter would
	// raise the message allowance on the strength of a division that never happened.
	TestEqual(TEXT("and no count to raise the allowance to"), Empty.MessagesAffordedByWindow, 0);

	// A drain short enough to measure as no time at all on a coarse clock. Time is the denominator, so a
	// reading taken from that alone would divide by zero rather than report a very cheap message.
	FCrowdyDrainCostMeter Instant;
	Instant.Record(5, 0.0);
	TestFalse(TEXT("messages delivered in no measured time are not yet a reading"), Instant.HasReading());

	// The assertion that matters, because time is the divisor here and not in the cost above: an unguarded
	// reading would divide by zero and clamp, offering the largest allowance an int32 can hold.
	TestEqual(TEXT("and no allowance is derived from them"),
		Instant.Read(ShippedDrainWindow).MessagesAffordedByWindow, 0);
	TestEqual(TEXT("nor any cost"), Instant.Read(ShippedDrainWindow).MicrosecondsPerMessage, 0.0, 0.0001);

	Meter.Record(1000, 0.002);
	TestTrue(TEXT("a recorded drain gives it a reading"), Meter.HasReading());

	// A window too short for one message affords none of it, rather than rounding up to one.
	TestEqual(TEXT("a window shorter than one message affords none"),
		Meter.Read(0.0000019).MessagesAffordedByWindow, 0);
	TestEqual(TEXT("a window of no length affords none"), Meter.Read(0.0).MessagesAffordedByWindow, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDrainCostResetStartsAFreshWindowTest,
	"CrowdySDK.CrowdyNet.DrainCostResetStartsAFreshWindow",
	CrowdyDrainCostTestFlags)

bool FCrowdyDrainCostResetStartsAFreshWindowTest::RunTest(const FString&)
{
	FCrowdyDrainCostMeter Meter;

	// Stands in for a run's warm-up, where the drain is expensive per message because it delivers few.
	Meter.Record(10, 0.001);
	TestEqual(TEXT("the warm-up reads as expensive"), Meter.Read(ShippedDrainWindow).MicrosecondsPerMessage,
		100.0, 0.0001);

	Meter.Reset();
	TestFalse(TEXT("a reset meter has nothing to report"), Meter.HasReading());

	Meter.Record(1000, 0.002);

	// 2.0, not the 2.97 a window still carrying the warm-up would give. A steady-state figure that inherits
	// the warm-up understates what the drain window affords, which is the direction that leaves capacity unused.
	TestEqual(TEXT("steady state is measured without the warm-up"),
		Meter.Read(ShippedDrainWindow).MicrosecondsPerMessage, 2.0, 0.0001);
	TestEqual(TEXT("and the affordable count follows it"),
		Meter.Read(ShippedDrainWindow).MessagesAffordedByWindow, 2000);

	// 1000 a drain, not 500. The drain count has to be reset with the other two or the average drain size
	// reads half its true value, which is the figure that says whether the allowance is being reached.
	TestEqual(TEXT("and the drain count was reset with them"),
		Meter.Read(ShippedDrainWindow).MessagesPerDrain, 1000.0, 0.0001);
	return true;
}

#endif
