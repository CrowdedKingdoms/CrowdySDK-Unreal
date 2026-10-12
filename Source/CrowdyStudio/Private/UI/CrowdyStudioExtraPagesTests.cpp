// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "CrowdyStudioModule.h"
#include "UI/CrowdyStudioPages.h"
#include "Widgets/SNullWidget.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStudioExtraPageUnregisterNotifiesTest,
	"CrowdySDK.CrowdyStudio.ExtraPageUnregisterNotifiesWhileListed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCrowdyStudioExtraPageUnregisterNotifiesTest::RunTest(const FString& /*Parameters*/)
{
	const FName Id(TEXT("CrowdyStudioTestPage"));
	auto IsListed = [Id]() { return CrowdyStudioExtraPages::GetPages().ContainsByPredicate([Id](const FCrowdyStudioExtraPage& Page) { return Page.Id == Id; }); };
	CrowdyStudioExtraPages::RegisterPage(Id, FText::FromString(TEXT("TEST")), FText::FromString(TEXT("Test Page")), TEXT("server"),
		[]() -> TSharedRef<SWidget> { return SNullWidget::NullWidget; });

	TArray<FName> Heard;
	bool bListedWhenHeard = false;
	const FDelegateHandle Handle = CrowdyStudioExtraPages::OnPageUnregistered().AddLambda([&](FName Unregistered)
	{
		Heard.Add(Unregistered);
		bListedWhenHeard = IsListed();
	});
	const int32 Listed = CrowdyStudioExtraPages::GetPages().IndexOfByPredicate([Id](const FCrowdyStudioExtraPage& Page) { return Page.Id == Id; });
	CrowdyStudioExtraPages::RegisterPage(Id, FText::FromString(TEXT("TEST")), FText::FromString(TEXT("Test Page Again")), TEXT("server"),
		[]() -> TSharedRef<SWidget> { return SNullWidget::NullWidget; });
	const int32 Again = CrowdyStudioExtraPages::GetPages().IndexOfByPredicate([Id](const FCrowdyStudioExtraPage& Page) { return Page.Id == Id; });
	TestTrue(TEXT("registering again replaces the page where it was, so open windows keep their slot order"), Again == Listed && Again != INDEX_NONE
		&& CrowdyStudioExtraPages::GetPages()[Again].Label.ToString() == TEXT("Test Page Again"));
	TestEqual(TEXT("and drops the old widget"), Heard.Num(), 1);
	Heard.Reset();

	CrowdyStudioExtraPages::UnregisterPage(Id);
	CrowdyStudioExtraPages::UnregisterPage(Id);
	CrowdyStudioExtraPages::UnregisterPage(FName(TEXT("CrowdyStudioNeverRegistered")));
	CrowdyStudioExtraPages::OnPageUnregistered().Remove(Handle);

	TestEqual(TEXT("unregistering a listed page is heard once, and a page that is not listed is not heard"), Heard.Num(), 1);
	TestTrue(TEXT("it is heard with the page's Id"), Heard.Num() == 1 && Heard[0] == Id);
	TestTrue(TEXT("it is heard while the page is still listed, so a window can drop the widget before its module unloads"), bListedWhenHeard);
	TestFalse(TEXT("the page is gone afterwards"), IsListed());
	return true;
}

#endif
