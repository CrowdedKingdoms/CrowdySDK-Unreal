#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Data/TInterpolatedField.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyInterpolatedFieldTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	float LerpFloat(const float A, const float B, const float Alpha)
	{
		return FMath::Lerp(A, B, Alpha);
	}
}

// A render clock that has not caught up with the oldest sample must hold that sample. Extrapolating from the newest
// pair instead ran the motion backwards, so a proxy snapped away from its first known value and back again once the
// clock caught up. The in-range and past-the-newest cases are the controls that show the field still interpolates
// and still extrapolates forward.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolatedFieldHoldsOldestSampleTest,
	"CrowdySDK.CrowdyReplication.InterpolatedFieldHoldsOldestSample", CrowdyInterpolatedFieldTestFlags)
bool FCrowdyInterpolatedFieldHoldsOldestSampleTest::RunTest(const FString& Parameters)
{
	TInterpolatedField<float, 4> Field;
	Field.Push(10.f, 1000);
	Field.Push(20.f, 2000);

	TestEqual(TEXT("a render time before the oldest sample holds the oldest sample"), Field.Sample(500, LerpFloat), 10.f);
	TestEqual(TEXT("the oldest timestamp itself holds the oldest sample"), Field.Sample(1000, LerpFloat), 10.f);
	TestEqual(TEXT("CONTROL: a render time between samples interpolates"), Field.Sample(1500, LerpFloat), 15.f);
	TestEqual(TEXT("CONTROL: a render time past the newest sample extrapolates forward"), Field.Sample(2100, LerpFloat), 21.f);
	return true;
}

// A sender that hitches can deliver its next two updates almost together. Extrapolating from that bunched pair at its
// own 1 ms spacing projected its step up to 200 times, so a remote copy flicked far away for a frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInterpolatedFieldBunchedPairTest,
	"CrowdySDK.CrowdyReplication.InterpolatedFieldBunchedPairDoesNotSpike", CrowdyInterpolatedFieldTestFlags)
bool FCrowdyInterpolatedFieldBunchedPairTest::RunTest(const FString& Parameters)
{
	TInterpolatedField<float, 8> Field;
	for (int32 Step = 0; Step < 5; ++Step)
	{
		Field.Push(10.f * Step, 1000 + 50 * Step);
	}
	Field.Push(50.f, 1201);

	const float Extrapolated = Field.Sample(1251, LerpFloat);
	TestTrue(FString::Printf(TEXT("50 ms past a bunched pair stays near the newest sample (got %.1f)"), Extrapolated),
		Extrapolated >= 50.f && Extrapolated <= 70.f);

	TInterpolatedField<float, 8> Even;
	for (int32 Step = 0; Step < 5; ++Step)
	{
		Even.Push(10.f * Step, 1000 + 50 * Step);
	}
	TestEqual(TEXT("CONTROL: evenly spaced samples still extrapolate at their own slope"), Even.Sample(1250, LerpFloat), 50.f);

	TInterpolatedField<float, 2> FreshPair;
	FreshPair.Push(0.f, 1000);
	FreshPair.Push(10.f, 1001);
	const float FromPair = FreshPair.Sample(1051, LerpFloat);
	TestTrue(FString::Printf(TEXT("a bunched first pair does not spike either (got %.1f)"), FromPair), FromPair <= 20.f);

	TInterpolatedField<float, 8> AfterIdle;
	AfterIdle.Push(0.f, 0);
	for (int32 Step = 0; Step < 5; ++Step)
	{
		AfterIdle.Push(10.f * Step, 3000 + 50 * Step);
	}
	TestEqual(TEXT("an old idle gap does not damp extrapolation once moving resumes"), AfterIdle.Sample(3250, LerpFloat), 50.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
