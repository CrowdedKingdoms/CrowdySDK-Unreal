// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "UI/SCrowdyBusyBar.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyBusyBarTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	constexpr float CrowdyBusyBarSweep = 1.0f; // one second each way, so the numbers below read as the clock
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBusyBarSweepIsContinuousTest,
	"CrowdySDK.CrowdyStudio.BusyBarSweepIsContinuous", CrowdyBusyBarTestFlags)

bool FCrowdyBusyBarSweepIsContinuousTest::RunTest(const FString& /*Parameters*/)
{
	// The turn is the whole difficulty. A fold-back that is off by a factor sends the pill back to the start instead
	// of returning it, and on screen that is indistinguishable from a dropped frame, so it is the one thing a paint
	// cannot be eyeballed for.
	TestEqual(TEXT("The sweep starts at the left"), SCrowdyBusyBar::PhaseAtTime(0.0, CrowdyBusyBarSweep), 0.0f);

	const float AtTurn = SCrowdyBusyBar::PhaseAtTime(CrowdyBusyBarSweep, CrowdyBusyBarSweep);
	TestTrue(TEXT("The sweep reaches the far end exactly once per pass"), FMath::IsNearlyEqual(AtTurn, 1.0f, 0.001f));

	// Either side of the turn the pill must be in nearly the same place. A jump here is the defect.
	const float JustBefore = SCrowdyBusyBar::PhaseAtTime(CrowdyBusyBarSweep - 0.001, CrowdyBusyBarSweep);
	const float JustAfter = SCrowdyBusyBar::PhaseAtTime(CrowdyBusyBarSweep + 0.001, CrowdyBusyBarSweep);
	TestTrue(TEXT("The pill returns rather than jumping at the turn"),
		FMath::Abs(JustBefore - JustAfter) < 0.01f);

	// And it comes back to where it started, so the cycle repeats seamlessly too.
	const float AtCycleEnd = SCrowdyBusyBar::PhaseAtTime(CrowdyBusyBarSweep * 2.0, CrowdyBusyBarSweep);
	TestTrue(TEXT("A full cycle returns to the left"), FMath::IsNearlyEqual(AtCycleEnd, 0.0f, 0.001f));

	// It genuinely moves: a bar that reports the same phase forever is exactly the bug this replaced, where the
	// animation ran and nothing on screen changed.
	TestTrue(TEXT("The sweep is somewhere else a quarter of the way through"),
		FMath::Abs(SCrowdyBusyBar::PhaseAtTime(CrowdyBusyBarSweep * 0.5, CrowdyBusyBarSweep) - 0.0f) > 0.1f);

	// Over a long run of arbitrary times it never leaves its range, whatever the clock says.
	for (int32 Step = 0; Step < 400; ++Step)
	{
		const double Time = 12345.678 + static_cast<double>(Step) * 0.037;
		const float Value = SCrowdyBusyBar::PhaseAtTime(Time, CrowdyBusyBarSweep);
		if (Value < 0.0f || Value > 1.0f)
		{
			AddError(FString::Printf(TEXT("Phase left its range at t=%f: %f"), Time, Value));
			break;
		}
	}

	// A clock reading before the origin folds to a negative remainder, which would put the pill off the track.
	const float BeforeOrigin = SCrowdyBusyBar::PhaseAtTime(-0.25, CrowdyBusyBarSweep);
	TestTrue(TEXT("A negative clock still yields a phase on the track"),
		BeforeOrigin >= 0.0f && BeforeOrigin <= 1.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBusyBarPillStaysOnTheTrackTest,
	"CrowdySDK.CrowdyStudio.BusyBarPillStaysOnTheTrack", CrowdyBusyBarTestFlags)

bool FCrowdyBusyBarPillStaysOnTheTrackTest::RunTest(const FString& /*Parameters*/)
{
	// The sweep is drawn with no clip, so an overhang paints outside the widget and over whatever sits beside it
	// rather than being trimmed. The travel has to be the track minus the pill for that never to happen.
	constexpr float Track = 120.0f;
	constexpr float Pill = 38.4f;

	TestEqual(TEXT("At the start the pill is flush with the left"),
		SCrowdyBusyBar::PillLeftForPhase(Track, Pill, 0.0f), 0.0f);

	const float AtEnd = SCrowdyBusyBar::PillLeftForPhase(Track, Pill, 1.0f);
	TestTrue(TEXT("At the end the pill is flush with the right"),
		FMath::IsNearlyEqual(AtEnd + Pill, Track, 0.01f));

	for (int32 Step = 0; Step <= 100; ++Step)
	{
		const float Phase = static_cast<float>(Step) / 100.0f;
		const float Left = SCrowdyBusyBar::PillLeftForPhase(Track, Pill, SCrowdyBusyBar::Ease(Phase));
		if (Left < 0.0f || Left + Pill > Track + 0.01f)
		{
			AddError(FString::Printf(TEXT("The pill left the track at phase %f: left=%f right=%f"),
				Phase, Left, Left + Pill));
			break;
		}
	}

	// A pill as wide as its track has nowhere to go, and must sit still rather than being driven off the end.
	TestEqual(TEXT("A pill filling the track does not move"),
		SCrowdyBusyBar::PillLeftForPhase(40.0f, 40.0f, 1.0f), 0.0f);

	// The easing has to arrive at both ends, or the pill never reaches them.
	TestEqual(TEXT("Easing starts at the start"), SCrowdyBusyBar::Ease(0.0f), 0.0f);
	TestEqual(TEXT("Easing finishes at the finish"), SCrowdyBusyBar::Ease(1.0f), 1.0f);
	TestTrue(TEXT("Easing is slower at the ends than in the middle"),
		SCrowdyBusyBar::Ease(0.1f) < 0.1f && SCrowdyBusyBar::Ease(0.9f) > 0.9f);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
