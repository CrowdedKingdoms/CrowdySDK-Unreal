// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Interpolation/CrowdyInterpolationTiming.h"

#include <limits>

// Every case here is an arrival pattern a real connection produces, driven through the estimator as a
// plain sequence of timestamps. What is being checked is always the same thing: does the segment get
// replayed over roughly the time it will take the next update to show up. Too short and the entity
// arrives early and stands still, which is the stutter this exists to remove.
namespace
{
	constexpr EAutomationTestFlags CrowdyInterpolationTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyInterpolationTuning CrowdyInterpolationTestTuning()
	{
		FCrowdyInterpolationTuning Tuning;
		Tuning.Overshoot = 1.2f;
		Tuning.MinDuration = 0.05f;
		Tuning.MaxDuration = 0.5f;
		Tuning.FirstUpdateDuration = 0.1f;
		Tuning.bAdaptive = true;
		return Tuning;
	}

	// Stands in for one remote entity's interpolation fragment across a run of arrivals.
	struct FCrowdyInterpolationTestEntity
	{
		FCrowdyArrivalIntervalHistory History;
		double LastArrivalSeconds = 0.0;

		FCrowdySegmentTiming Arrive(const double AtSeconds, const FCrowdyInterpolationTuning& Tuning, const float DeclaredDuration = -1.0f)
		{
			return CrowdyInterpolationTiming::OpenSegment(History, LastArrivalSeconds, AtSeconds, DeclaredDuration, Tuning);
		}
	};
}

// Nothing has arrived before the first update, so there is no interval to measure and the project
// setting is all there is to go on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationFirstUpdateTest,
	"CrowdySDK.Mass.InterpolationFirstUpdateUsesTheSetting", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationFirstUpdateTest::RunTest(const FString& Parameters)
{
	const FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();
	FCrowdyInterpolationTestEntity Entity;

	const FCrowdySegmentTiming First = Entity.Arrive(100.0, Tuning);

	TestEqual(TEXT("The first segment falls back to the project setting"), First.Duration, 0.1f);
	TestEqual(TEXT("Nothing is known about real speed yet, so it is reported as-is"), First.VelocityScale, 1.0f);

	return true;
}

// A sender ticking steadily is the ordinary case, and the whole point of the change: the segment has
// to be replayed over about the interval it took to arrive, not over a fixed span unrelated to it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationSteadyStreamTest,
	"CrowdySDK.Mass.InterpolationFollowsASteadyStream", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationSteadyStreamTest::RunTest(const FString& Parameters)
{
	const FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();
	FCrowdyInterpolationTestEntity Entity;

	// A 100 ms cadence, which is the 10 Hz sender the crowd harness drives.
	FCrowdySegmentTiming Timing;
	for (int32 Update = 0; Update < 8; ++Update)
	{
		Timing = Entity.Arrive(100.0 + Update * 0.1, Tuning);
	}

	TestEqual(TEXT("A 100 ms cadence is replayed over the overshot interval"), Timing.Duration, 0.12f, 0.001f);
	TestEqual(TEXT("Reported speed is corrected back to the real one"), Timing.VelocityScale, 1.2f, 0.01f);

	// The claim that matters is not the exact number but that the segment outlasts the gap to the next
	// update, because that is what stops the entity arriving early and standing still.
	TestTrue(TEXT("The segment outlasts the interval, so the next update lands mid-flight"), Timing.Duration > 0.1f);

	return true;
}

// A connection whose arrivals are spread out has to be given more headroom than one whose arrivals
// are even, and without being told which it is. This is the case a fixed multiplier on the typical
// interval gets wrong, and it was found on a real 240 ms transit route rather than reasoned about:
// segments timed against the middle sample finished fractionally early and the entities lurched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationGivesJitterMoreHeadroomTest,
	"CrowdySDK.Mass.InterpolationGivesJitterMoreHeadroom", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationGivesJitterMoreHeadroomTest::RunTest(const FString& Parameters)
{
	const FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();

	// A nominal 10 Hz stream that does not keep time: 0.09, 0.10, 0.10, 0.12 and 0.20 apart.
	const double Arrivals[] = { 100.0, 100.09, 100.19, 100.29, 100.41, 100.61 };

	FCrowdyInterpolationTestEntity Jittery;
	FCrowdySegmentTiming Timing;
	for (const double At : Arrivals)
	{
		Timing = Jittery.Arrive(At, Tuning);
	}

	// The middle sample is 0.10, so planning against it would give 0.12 s and leave every interval
	// longer than 0.10 finishing early. The second largest is 0.12, which covers all but the one
	// dropped-looking outlier.
	TestEqual(TEXT("A jittery stream is planned against its spread, not its middle"), Timing.Duration, 0.144f, 0.001f);
	TestTrue(TEXT("That is more headroom than the middle sample would have given"), Timing.Duration > 0.12f);

	// The same nominal rate, arriving evenly, must NOT pay for headroom it does not need.
	FCrowdyInterpolationTestEntity Even;
	FCrowdySegmentTiming EvenTiming;
	for (int32 Update = 0; Update < 6; ++Update)
	{
		EvenTiming = Even.Arrive(100.0 + Update * 0.1, Tuning);
	}

	TestEqual(TEXT("An even stream at the same rate stays tight"), EvenTiming.Duration, 0.12f, 0.001f);
	TestTrue(TEXT("The jittery connection is given the longer segment of the two"), Timing.Duration > EvenTiming.Duration);

	return true;
}

// Before there are enough samples to discard an outlier, the estimate leans long rather than short.
// Erring long costs a few milliseconds of lag; erring short costs a visible stall, and only one of
// those is worth risking while nothing is known about the connection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationLeansLongWhileLearningTest,
	"CrowdySDK.Mass.InterpolationLeansLongWhileLearning", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationLeansLongWhileLearningTest::RunTest(const FString& Parameters)
{
	const FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();
	FCrowdyInterpolationTestEntity Entity;

	Entity.Arrive(100.0, Tuning);

	// One interval known, 0.20 s.
	const FCrowdySegmentTiming AfterOne = Entity.Arrive(100.2, Tuning);
	TestEqual(TEXT("A single known interval is used as it stands"), AfterOne.Duration, 0.24f, 0.001f);

	// A second, shorter interval must not pull the estimate down to it while the sample is this thin.
	const FCrowdySegmentTiming AfterTwo = Entity.Arrive(100.3, Tuning);
	TestEqual(TEXT("With two samples the longer one wins"), AfterTwo.Duration, 0.24f, 0.001f);

	return true;
}

// A sender at twice the rate has to be followed just as closely. Nothing about the estimate is tied to
// a particular cadence, which is what a fixed setting got wrong.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationFollowsARateChangeTest,
	"CrowdySDK.Mass.InterpolationFollowsARateChange", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationFollowsARateChangeTest::RunTest(const FString& Parameters)
{
	const FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();
	FCrowdyInterpolationTestEntity Entity;

	double Now = 100.0;
	for (int32 Update = 0; Update < 8; ++Update)
	{
		Entity.Arrive(Now, Tuning);
		Now += 0.1;
	}

	// The history is five deep, so six updates at the new rate is enough for it to have turned over.
	FCrowdySegmentTiming Timing;
	for (int32 Update = 0; Update < 6; ++Update)
	{
		Timing = Entity.Arrive(Now, Tuning);
		Now += 0.2;
	}

	TestEqual(TEXT("A 200 ms cadence is replayed over the overshot interval"), Timing.Duration, 0.24f, 0.001f);

	return true;
}

// A dropped update looks exactly like one doubled interval. This is the case a mean handles badly and a
// median ignores outright, and it is why the route this ships on matters: long routes lose packets.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationAbsorbsADroppedUpdateTest,
	"CrowdySDK.Mass.InterpolationAbsorbsADroppedUpdate", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationAbsorbsADroppedUpdateTest::RunTest(const FString& Parameters)
{
	const FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();
	FCrowdyInterpolationTestEntity Entity;

	double Now = 100.0;
	for (int32 Update = 0; Update < 6; ++Update)
	{
		Entity.Arrive(Now, Tuning);
		Now += 0.1;
	}

	// One update goes missing, so the next lands a full two intervals late.
	Now += 0.1;
	const FCrowdySegmentTiming AfterDrop = Entity.Arrive(Now, Tuning);

	TestEqual(TEXT("One lost update does not move the estimate at all"), AfterDrop.Duration, 0.12f, 0.001f);

	// A mean of the same five samples would have been pulled to 0.12 s and the duration to 0.144 s, so
	// this margin is what distinguishes the two estimators rather than an arbitrary tolerance.
	TestTrue(TEXT("The estimate stays below what an average of the same samples would give"), AfterDrop.Duration < 0.14f);

	return true;
}

// Two updates draining in the same frame report an interval of nothing at all. Left alone that would
// ask for an instantaneous segment, which is a snap.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationAbsorbsABurstTest,
	"CrowdySDK.Mass.InterpolationAbsorbsABurst", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationAbsorbsABurstTest::RunTest(const FString& Parameters)
{
	const FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();
	FCrowdyInterpolationTestEntity Entity;

	double Now = 100.0;
	for (int32 Update = 0; Update < 6; ++Update)
	{
		Entity.Arrive(Now, Tuning);
		Now += 0.1;
	}

	// Same frame as the previous arrival, so no time has passed between them.
	const FCrowdySegmentTiming Burst = Entity.Arrive(Now - 0.1, Tuning);

	TestEqual(TEXT("A burst of two in one frame does not move the estimate"), Burst.Duration, 0.12f, 0.001f);

	return true;
}

// A sender that stops entirely must not leave its entity gliding for as long as the silence lasts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationClampsAStallTest,
	"CrowdySDK.Mass.InterpolationClampsAStall", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationClampsAStallTest::RunTest(const FString& Parameters)
{
	const FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();
	FCrowdyInterpolationTestEntity Entity;

	// Long enough for the whole history to be nothing but silences.
	double Now = 100.0;
	FCrowdySegmentTiming Timing;
	for (int32 Update = 0; Update < 8; ++Update)
	{
		Timing = Entity.Arrive(Now, Tuning);
		Now += 4.0;
	}

	TestEqual(TEXT("A long silence is bounded by the ceiling"), Timing.Duration, Tuning.MaxDuration);

	// The scale reports what is genuinely happening: the entity is being shown moving far slower than
	// the sender did, because a four second segment is being crossed in half a second.
	TestTrue(TEXT("The reported speed correction stays below one when the ceiling bites"), Timing.VelocityScale < 1.0f);

	return true;
}

// A sender naming its own duration is describing how it wants the segment played, so it wins. It still
// arrives over the wire, so it is still bounded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationDeclaredDurationWinsTest,
	"CrowdySDK.Mass.InterpolationDeclaredDurationWinsButIsBounded", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationDeclaredDurationWinsTest::RunTest(const FString& Parameters)
{
	const FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();
	FCrowdyInterpolationTestEntity Entity;

	double Now = 100.0;
	for (int32 Update = 0; Update < 6; ++Update)
	{
		Entity.Arrive(Now, Tuning);
		Now += 0.1;
	}

	const FCrowdySegmentTiming Declared = Entity.Arrive(Now, Tuning, 0.3f);
	TestEqual(TEXT("A declared duration is used in place of the estimate"), Declared.Duration, 0.3f);

	Now += 0.1;
	const FCrowdySegmentTiming Forged = Entity.Arrive(Now, Tuning, 30.0f);
	TestEqual(TEXT("A declared duration above the ceiling is refused"), Forged.Duration, Tuning.MaxDuration);

	// The floor exists to stop a burst of arrivals producing an instant segment. A sender that asks for
	// one on purpose is doing something else, and a teleport is the ordinary reason to.
	Now += 0.1;
	const FCrowdySegmentTiming Snap = Entity.Arrive(Now, Tuning, 0.0f);
	TestEqual(TEXT("A sender that asks to snap is allowed to"), Snap.Duration, 0.0f);

	// Arrivals keep being recorded through the declared stretch, so the estimate is ready the moment
	// the sender stops naming one rather than starting again from the project setting.
	Now += 0.1;
	const FCrowdySegmentTiming BackToEstimate = Entity.Arrive(Now, Tuning);
	TestEqual(TEXT("The estimate survives a stretch of declared durations"), BackToEstimate.Duration, 0.12f, 0.001f);

	return true;
}

// Turning the estimate off has to restore exactly the fixed duration that predates it, or the setting
// is not the comparison it claims to be.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationCanBeTurnedOffTest,
	"CrowdySDK.Mass.InterpolationCanBeTurnedOff", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationCanBeTurnedOffTest::RunTest(const FString& Parameters)
{
	FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();
	Tuning.bAdaptive = false;

	FCrowdyInterpolationTestEntity Entity;

	double Now = 100.0;
	FCrowdySegmentTiming Timing;
	for (int32 Update = 0; Update < 8; ++Update)
	{
		Timing = Entity.Arrive(Now, Tuning);
		Now += 0.13;
	}

	TestEqual(TEXT("Every segment uses the fixed duration"), Timing.Duration, 0.1f);
	TestEqual(TEXT("Nothing is claimed about real speed"), Timing.VelocityScale, 1.0f);

	return true;
}

// Timing is derived from values that arrive over the wire, so a non-finite one is reachable. One that
// reached the history would sit in it and defeat every comparison made against it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolationRefusesNonFiniteInputTest,
	"CrowdySDK.Mass.InterpolationRefusesNonFiniteInput", CrowdyInterpolationTestFlags)
bool FCrowdyInterpolationRefusesNonFiniteInputTest::RunTest(const FString& Parameters)
{
	const FCrowdyInterpolationTuning Tuning = CrowdyInterpolationTestTuning();

	FCrowdyArrivalIntervalHistory History;
	History.Record(std::numeric_limits<float>::quiet_NaN());
	History.Record(std::numeric_limits<float>::infinity());
	History.Record(-1.0f);

	TestTrue(TEXT("None of them are kept"), History.IsEmpty());
	TestEqual(TEXT("An empty history has no opinion"), History.PlanningInterval(), 0.0f);

	History.Record(0.1f);
	TestEqual(TEXT("A usable interval still lands"), History.PlanningInterval(), 0.1f);

	// A backwards clock produces an arrival earlier than the one before it, which would otherwise be a
	// negative interval.
	FCrowdyInterpolationTestEntity Entity;
	Entity.Arrive(100.0, Tuning);
	const FCrowdySegmentTiming Backwards = Entity.Arrive(50.0, Tuning);

	TestEqual(TEXT("An arrival that goes backwards in time is not measured"), Backwards.Duration, Tuning.FirstUpdateDuration);

	return true;
}

#endif
