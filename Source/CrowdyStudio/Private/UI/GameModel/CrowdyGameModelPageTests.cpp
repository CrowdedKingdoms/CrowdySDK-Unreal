// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Model/CrowdyStudioTypes.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyGameModelPageTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyPreSeedCardTabsTest,
	"CrowdySDK.CrowdyStudio.PreSeedCardFollowsTheTab", CrowdyGameModelPageTestFlags)

bool FCrowdyPreSeedCardTabsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("The pre-seed card belongs over the Models tab"), CrowdyPreSeedCardBelongsOnTab(TEXT("models")));
	TestTrue(TEXT("The pre-seed card belongs over the Advanced tab"), CrowdyPreSeedCardBelongsOnTab(TEXT("advanced")));
	TestTrue(TEXT("The pre-seed card belongs over the Issues tab"), CrowdyPreSeedCardBelongsOnTab(TEXT("issues")));
	TestFalse(TEXT("The pre-seed card is collapsed over the Live tab"), CrowdyPreSeedCardBelongsOnTab(TEXT("live")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
