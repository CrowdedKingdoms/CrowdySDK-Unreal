// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/CrowdyConsentPromptSubsystem.h"

#include "CrowdyServicesLog.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Subsystem/CrowdyAuthentication.h"
#include "UI/SCrowdyConsentPrompt.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

namespace
{
	constexpr int32 PromptZOrder = 10000;
}

void UCrowdyConsentPromptSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (!GetGameInstance())
	{
		return;
	}

	UCrowdyAuthentication* Authentication = Collection.InitializeDependency<UCrowdyAuthentication>();
	if (!Authentication)
	{
		return;
	}

	Authentication->OnLegalAcceptanceRequired.AddDynamic(this, &UCrowdyConsentPromptSubsystem::HandleLegalAcceptanceRequired);
	Authentication->OnLegalAcceptanceEnded.AddDynamic(this, &UCrowdyConsentPromptSubsystem::HandleLegalAcceptanceEnded);
}

UCrowdyAuthentication* UCrowdyConsentPromptSubsystem::GetAuthentication() const
{
	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyAuthentication>() : nullptr;
}

void UCrowdyConsentPromptSubsystem::Deinitialize()
{
	if (UCrowdyAuthentication* Authentication = GetAuthentication())
	{
		Authentication->OnLegalAcceptanceRequired.RemoveAll(this);
		Authentication->OnLegalAcceptanceEnded.RemoveAll(this);
	}

	ClosePrompt();
	Super::Deinitialize();
}

void UCrowdyConsentPromptSubsystem::HandleLegalAcceptanceRequired()
{
	// Something else took it off the viewport (a "remove all widgets"), so this one is closed, not open. A preview
	// gives way to the real thing.
	if (Prompt.IsValid() && (bPreview || !Prompt->GetParentWidget().IsValid()))
	{
		ClosePrompt();
	}
	if (Prompt.IsValid() || !GetDefault<UCrowdySDKDeveloperSettings>()->bShowConsentPrompt)
	{
		return;
	}

	OpenPrompt(false);
}

void UCrowdyConsentPromptSubsystem::ShowPreview()
{
	if (Prompt.IsValid() && !Prompt->GetParentWidget().IsValid())
	{
		ClosePrompt();
	}
	if (Prompt.IsValid())
	{
		return;
	}

	OpenPrompt(true);
}

void UCrowdyConsentPromptSubsystem::OpenPrompt(bool bInPreview)
{
	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* ViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	if (!ViewportClient || !FSlateApplication::IsInitialized() || !FApp::CanEverRender())
	{
		return;
	}

	Prompt = SNew(SCrowdyConsentPrompt)
		.OnAccept(FSimpleDelegate::CreateUObject(this, &UCrowdyConsentPromptSubsystem::HandleAccept))
		.OnDecline(FSimpleDelegate::CreateUObject(this, &UCrowdyConsentPromptSubsystem::HandleDecline));
	ViewportClient->AddViewportWidgetContent(Prompt.ToSharedRef(), PromptZOrder);

	bPreview = bInPreview;
	Viewport = ViewportClient;
	bPreviousIgnoreInput = ViewportClient->IgnoreInput();
	PreviousCaptureMode = ViewportClient->GetMouseCaptureMode();
	PreviousLockMode = ViewportClient->GetMouseLockMode();
	ViewportClient->SetIgnoreInput(true);
	ViewportClient->SetMouseCaptureMode(EMouseCaptureMode::NoCapture);
	ViewportClient->SetMouseLockMode(EMouseLockMode::DoNotLock);

	// A game-only input mode has the pointer captured and locked; the prompt cannot be clicked until it is let go.
	FSlateApplication& SlateApp = FSlateApplication::Get();
	SlateApp.ReleaseAllPointerCapture();
	PreviousFocus = SlateApp.GetUserFocusedWidget(0);
	SlateApp.SetUserFocus(0, Prompt->GetInitialFocus(), EFocusCause::SetDirectly);
}

void UCrowdyConsentPromptSubsystem::HandleAccept()
{
	// A preview records nothing.
	if (bPreview)
	{
		ClosePrompt();
		return;
	}

	UCrowdyAuthentication* Authentication = GetAuthentication();
	if (!Prompt.IsValid() || !Authentication)
	{
		return;
	}

	Prompt->SetBusy(true);

	FOnLegalConsentsRecorded OnRecorded;
	OnRecorded.BindDynamic(this, &UCrowdyConsentPromptSubsystem::HandleConsentsRecorded);
	FOnAuthError OnError;
	OnError.BindDynamic(this, &UCrowdyConsentPromptSubsystem::HandleConsentRecordFailed);
	Authentication->RecordPlayerConsents(true, true, OnRecorded, OnError);
}

void UCrowdyConsentPromptSubsystem::HandleDecline()
{
	const bool bWasPreview = bPreview;
	ClosePrompt();
	if (bWasPreview)
	{
		return;
	}

	if (UCrowdyAuthentication* Authentication = GetAuthentication())
	{
		Authentication->DeclineLegalAcceptance();
	}
}

void UCrowdyConsentPromptSubsystem::HandleLegalAcceptanceEnded()
{
	ClosePrompt();
}

void UCrowdyConsentPromptSubsystem::HandleConsentsRecorded()
{
	ClosePrompt();
}

void UCrowdyConsentPromptSubsystem::HandleConsentRecordFailed(FString Message)
{
	if (!Prompt.IsValid())
	{
		return;
	}

	Prompt->SetBusy(false);
	Prompt->SetError(FText::FromString(Message));
}

void UCrowdyConsentPromptSubsystem::ClosePrompt()
{
	if (!Prompt.IsValid())
	{
		return;
	}

	UGameViewportClient* ViewportClient = Viewport.Get();
	if (ViewportClient)
	{
		ViewportClient->RemoveViewportWidgetContent(Prompt.ToSharedRef());

		// Restored only where nothing else changed it while the prompt was up.
		if (ViewportClient->IgnoreInput())
		{
			ViewportClient->SetIgnoreInput(bPreviousIgnoreInput);
		}
		if (ViewportClient->GetMouseCaptureMode() == EMouseCaptureMode::NoCapture)
		{
			ViewportClient->SetMouseCaptureMode(PreviousCaptureMode);
		}
		if (ViewportClient->GetMouseLockMode() == EMouseLockMode::DoNotLock)
		{
			ViewportClient->SetMouseLockMode(PreviousLockMode);
		}
	}

	Prompt.Reset();
	bPreview = false;

	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	if (const TSharedPtr<SWidget> Focus = PreviousFocus.Pin())
	{
		FSlateApplication::Get().SetUserFocus(0, Focus, EFocusCause::SetDirectly);
		return;
	}
	if (ViewportClient && !bPreviousIgnoreInput)
	{
		FSlateApplication::Get().SetUserFocus(0, ViewportClient->GetGameViewportWidget(), EFocusCause::SetDirectly);
	}
}

#if !UE_BUILD_SHIPPING
static void PreviewConsentPrompt(UWorld* World)
{
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UCrowdyConsentPromptSubsystem* Host = GameInstance ? GameInstance->GetSubsystem<UCrowdyConsentPromptSubsystem>() : nullptr;
	if (!Host)
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("crowdy.auth.consent.preview needs a running game instance."));
		return;
	}
	Host->ShowPreview();
}

static FAutoConsoleCommandWithWorld GCrowdyConsentPreviewCommand(
	TEXT("crowdy.auth.consent.preview"),
	TEXT("Shows the consent prompt over the game viewport without recording anything; Accept and Decline only close it."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&PreviewConsentPrompt));
#endif
