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

#endif // WITH_DEV_AUTOMATION_TESTS
