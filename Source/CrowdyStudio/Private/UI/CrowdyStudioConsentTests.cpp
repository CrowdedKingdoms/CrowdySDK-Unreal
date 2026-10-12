// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "CrowdyCppClient.h"
#include "UI/CrowdyStudioConsent.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStudioConsentTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FCrowdyCppAppTokenResult MakeRefusal(const TCHAR* Code)
	{
		FCrowdyCppAppTokenResult Result;
		Result.bOk = false;
		Result.ErrorCode = Code;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioConsentAsksOnRefusalTest,
	"CrowdySDK.Studio.Consent.AsksOnlyForTheConsentRefusal", CrowdyStudioConsentTestFlags)

bool FCrowdyStudioConsentAsksOnRefusalTest::RunTest(const FString& /*Parameters*/)
{
	TestTrue(TEXT("The consent refusal asks the player"),
		CrowdyStudioConsent::ShouldAskConsent(MakeRefusal(CrowdyCppLegalAcceptanceRequiredCode), false));

	TestFalse(TEXT("A different server code is a plain failure"),
		CrowdyStudioConsent::ShouldAskConsent(MakeRefusal(TEXT("UNAUTHENTICATED")), false));
	TestFalse(TEXT("A failure with no code is a plain failure"),
		CrowdyStudioConsent::ShouldAskConsent(MakeRefusal(TEXT("")), false));
	TestFalse(TEXT("The code is matched case-sensitively"),
		CrowdyStudioConsent::ShouldAskConsent(MakeRefusal(TEXT("legal_acceptance_required")), false));
	TestFalse(TEXT("A code embedded in a longer one is not the refusal"),
		CrowdyStudioConsent::ShouldAskConsent(MakeRefusal(TEXT("LEGAL_ACCEPTANCE_REQUIRED_X")), false));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioConsentAsksOnceTest,
	"CrowdySDK.Studio.Consent.RetriesOnlyOnce", CrowdyStudioConsentTestFlags)

bool FCrowdyStudioConsentAsksOnceTest::RunTest(const FString& /*Parameters*/)
{
	TestFalse(TEXT("A second refusal after the prompt is a plain failure"),
		CrowdyStudioConsent::ShouldAskConsent(MakeRefusal(CrowdyCppLegalAcceptanceRequiredCode), true));

	FCrowdyCppAppTokenResult Minted = MakeRefusal(CrowdyCppLegalAcceptanceRequiredCode);
	Minted.bOk = true;
	TestFalse(TEXT("A successful mint never asks"), CrowdyStudioConsent::ShouldAskConsent(Minted, false));

	return true;
}

#endif
