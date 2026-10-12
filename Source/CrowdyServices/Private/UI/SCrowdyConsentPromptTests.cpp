// Fill out your copyright notice in the Description page of Project Settings.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Children.h"
#include "Misc/AutomationTest.h"
#include "Styling/SlateTypes.h"
#include "UI/CrowdyBrandStyle.h"
#include "UI/CrowdyConsentPromptSubsystem.h"
#include "UI/SCrowdyConsentPrompt.h"
#include "Widgets/Text/STextBlock.h"

namespace CrowdyConsentPromptTestSupport
{
	constexpr EAutomationTestFlags PromptTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	bool HasVisibleText(const TSharedRef<SWidget>& Widget, const FText& Wanted)
	{
		if (!Widget->GetVisibility().IsVisible())
		{
			return false;
		}

		if (Widget->GetType() == FName(TEXT("STextBlock")))
		{
			return StaticCastSharedRef<STextBlock>(Widget)->GetText().EqualTo(Wanted);
		}

		FChildren* Children = Widget->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			if (HasVisibleText(Children->GetChildAt(Index), Wanted))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConsentAcceptNeedsBothBoxes,
	"CrowdySDK.Services.ConsentPrompt.AcceptNeedsBothBoxes", CrowdyConsentPromptTestSupport::PromptTestFlags)

bool FCrowdyConsentAcceptNeedsBothBoxes::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate is not initialised in this run; skipped"));
		return true;
	}

	TSharedRef<SCrowdyConsentPrompt> Prompt = SNew(SCrowdyConsentPrompt);
	TestFalse(TEXT("Accept is disabled with nothing ticked"), Prompt->CanAccept());

	Prompt->SetLegalChecked(true);
	TestFalse(TEXT("Accept is disabled with only the terms ticked"), Prompt->CanAccept());

	Prompt->SetLegalChecked(false);
	Prompt->SetAgeChecked(true);
	TestFalse(TEXT("Accept is disabled with only the age box ticked"), Prompt->CanAccept());

	Prompt->SetLegalChecked(true);
	TestTrue(TEXT("Accept is enabled once both boxes are ticked"), Prompt->CanAccept());

	Prompt->SetBusy(true);
	TestFalse(TEXT("Accept is disabled while busy"), Prompt->CanAccept());

	Prompt->SetBusy(false);
	TestTrue(TEXT("Accept is enabled again when no longer busy"), Prompt->CanAccept());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConsentAcceptFiresOnce,
	"CrowdySDK.Services.ConsentPrompt.AcceptFiresOnce", CrowdyConsentPromptTestSupport::PromptTestFlags)

bool FCrowdyConsentAcceptFiresOnce::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate is not initialised in this run; skipped"));
		return true;
	}

	int32 Accepted = 0;
	int32 Declined = 0;
	TSharedRef<SCrowdyConsentPrompt> Prompt = SNew(SCrowdyConsentPrompt)
		.OnAccept(FSimpleDelegate::CreateLambda([&Accepted]() { ++Accepted; }))
		.OnDecline(FSimpleDelegate::CreateLambda([&Declined]() { ++Declined; }));

	Prompt->Accept();
	TestEqual(TEXT("Accept does nothing before both boxes are ticked"), Accepted, 0);

	Prompt->SetLegalChecked(true);
	Prompt->SetAgeChecked(true);
	Prompt->Accept();
	TestEqual(TEXT("Accept fires OnAccept once"), Accepted, 1);
	TestEqual(TEXT("Accept does not fire OnDecline"), Declined, 0);

	Prompt->SetBusy(true);
	Prompt->Accept();
	TestEqual(TEXT("Accept does nothing while busy"), Accepted, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConsentDeclineFires,
	"CrowdySDK.Services.ConsentPrompt.DeclineFires", CrowdyConsentPromptTestSupport::PromptTestFlags)

bool FCrowdyConsentDeclineFires::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate is not initialised in this run; skipped"));
		return true;
	}

	int32 Accepted = 0;
	int32 Declined = 0;
	TSharedRef<SCrowdyConsentPrompt> Prompt = SNew(SCrowdyConsentPrompt)
		.OnAccept(FSimpleDelegate::CreateLambda([&Accepted]() { ++Accepted; }))
		.OnDecline(FSimpleDelegate::CreateLambda([&Declined]() { ++Declined; }));

	Prompt->Decline();
	TestEqual(TEXT("Decline fires OnDecline once with nothing ticked"), Declined, 1);
	TestEqual(TEXT("Decline does not fire OnAccept"), Accepted, 0);

	Prompt->SetBusy(true);
	Prompt->Decline();
	TestEqual(TEXT("Decline does nothing while a save is in flight"), Declined, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConsentSetErrorShowsText,
	"CrowdySDK.Services.ConsentPrompt.SetErrorShowsText", CrowdyConsentPromptTestSupport::PromptTestFlags)

bool FCrowdyConsentSetErrorShowsText::RunTest(const FString& Parameters)
{
	using namespace CrowdyConsentPromptTestSupport;

	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate is not initialised in this run; skipped"));
		return true;
	}

	const FText Message = FText::FromString(TEXT("Could not save your agreement."));
	TSharedRef<SCrowdyConsentPrompt> Prompt = SNew(SCrowdyConsentPrompt);
	TestFalse(TEXT("No error line before an error is set"), HasVisibleText(Prompt, Message));

	Prompt->SetError(Message);
	TestTrue(TEXT("The error text is on screen once set"), HasVisibleText(Prompt, Message));

	Prompt->SetBusy(true);
	TestFalse(TEXT("A new attempt clears the old error"), HasVisibleText(Prompt, Message));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConsentTickedBoxShowsATick,
	"CrowdySDK.Services.ConsentPrompt.TickedBoxShowsATick", CrowdyConsentPromptTestSupport::PromptTestFlags)

bool FCrowdyConsentTickedBoxShowsATick::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate is not initialised in this run; skipped"));
		return true;
	}

	const FCheckBoxStyle& CheckBox = FCrowdyBrandStyle::Get().GetWidgetStyle<FCheckBoxStyle>("Crowdy.CheckBox");
	TestTrue(TEXT("A ticked box draws the engine's tick image"),
		CheckBox.CheckedImage.GetResourceName().ToString().EndsWith(TEXT("check.svg")));
	TestTrue(TEXT("The tick is the brand gold"), CheckBox.CheckedImage.TintColor.GetSpecifiedColor().Equals(FCrowdyBrandStyle::Gold()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConsentFocusStartsOnATickBox,
	"CrowdySDK.Services.ConsentPrompt.FocusStartsOnATickBox", CrowdyConsentPromptTestSupport::PromptTestFlags)

bool FCrowdyConsentFocusStartsOnATickBox::RunTest(const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddInfo(TEXT("Slate is not initialised in this run; skipped"));
		return true;
	}

	// The prompt itself fills the screen, so focus on it leaves a gamepad with nothing to move to.
	TSharedRef<SCrowdyConsentPrompt> Prompt = SNew(SCrowdyConsentPrompt);
	TestEqual(TEXT("Focus starts on a tick box"), Prompt->GetInitialFocus()->GetType(), FName(TEXT("SCheckBox")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyConsentNoViewportNoPrompt,
	"CrowdySDK.Services.ConsentPrompt.NoViewportNoPrompt", CrowdyConsentPromptTestSupport::PromptTestFlags)

bool FCrowdyConsentNoViewportNoPrompt::RunTest(const FString& Parameters)
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UCrowdyConsentPromptSubsystem* Host = NewObject<UCrowdyConsentPromptSubsystem>(GameInstance);

	Host->HandleLegalAcceptanceRequired();
	TestFalse(TEXT("A game instance with no viewport opens no prompt"), Host->IsPromptOpen());
	return true;
}

#endif
