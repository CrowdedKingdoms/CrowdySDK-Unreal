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
#include "UI/SCrowdyAgePrompt.h"
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
	Authentication->OnAgeRequired.AddDynamic(this, &UCrowdyConsentPromptSubsystem::HandleAgeRequired);
	Authentication->OnAgeEntryEnded.AddDynamic(this, &UCrowdyConsentPromptSubsystem::HandleAgeEntryEnded);
}

UCrowdyAuthentication* UCrowdyConsentPromptSubsystem::GetAuthentication() const
{
#if WITH_DEV_AUTOMATION_TESTS
	if (UCrowdyAuthentication* Override = AuthenticationForTest.Get())
	{
		return Override;
	}
#endif

	const UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UCrowdyAuthentication>() : nullptr;
}

void UCrowdyConsentPromptSubsystem::Deinitialize()
{
	if (UCrowdyAuthentication* Authentication = GetAuthentication())
	{
		Authentication->OnLegalAcceptanceRequired.RemoveAll(this);
		Authentication->OnLegalAcceptanceEnded.RemoveAll(this);
		Authentication->OnAgeRequired.RemoveAll(this);
		Authentication->OnAgeEntryEnded.RemoveAll(this);
	}

	ClosePrompt();
	Super::Deinitialize();
}

void UCrowdyConsentPromptSubsystem::HandleLegalAcceptanceRequired()
{
	// Something else took it off the viewport (a "remove all widgets"), so this one is closed, not open. A preview
	// gives way to the real thing.
	if (Shown.IsValid() && (bPreview || !Shown->GetParentWidget().IsValid()))
	{
		ClosePrompt();
	}
	if (Shown.IsValid() || !GetDefault<UCrowdySDKDeveloperSettings>()->bShowConsentPrompt)
	{
		return;
	}

	if (OpenPrompt(false))
	{
		return;
	}

	// With no UI here and no other listener to ask the player, the held sign-in would wait forever.
	UCrowdyAuthentication* Authentication = GetAuthentication();
	if (!Authentication)
	{
		return;
	}
	const TArray<UObject*> Listeners = Authentication->OnLegalAcceptanceRequired.GetAllObjects();
	if (Listeners.Num() != 1 || Listeners[0] != this)
	{
		return;
	}

	UE_LOG(LogCrowdyServices, Warning,
		TEXT("[CrowdyAuth] This process has no UI to ask the player for consent; failing the held sign-in."));
	Authentication->DeclineLegalAcceptance();
}

void UCrowdyConsentPromptSubsystem::ShowPreview()
{
	if (Shown.IsValid() && !Shown->GetParentWidget().IsValid())
	{
		ClosePrompt();
	}
	if (Shown.IsValid())
	{
		return;
	}

	OpenPrompt(true);
}

bool UCrowdyConsentPromptSubsystem::OpenPrompt(bool bInPreview)
{
	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* ViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	if (!ViewportClient || !FSlateApplication::IsInitialized() || !FApp::CanEverRender())
	{
		return false;
	}

	Prompt = SNew(SCrowdyConsentPrompt)
		.OnAccept(FSimpleDelegate::CreateUObject(this, &UCrowdyConsentPromptSubsystem::HandleAccept))
		.OnDecline(FSimpleDelegate::CreateUObject(this, &UCrowdyConsentPromptSubsystem::HandleDecline));
	ShowWidget(*ViewportClient, Prompt.ToSharedRef(), Prompt->GetInitialFocus());
	bPreview = bInPreview;

	UCrowdyAuthentication* Authentication = GetAuthentication();
	if (!bInPreview && Authentication)
	{
		Prompt->SetLoadingDocuments(true);
		FOnLegalDocuments OnDocuments;
		OnDocuments.BindDynamic(this, &UCrowdyConsentPromptSubsystem::HandleLegalDocuments);
		Authentication->RequestLegalDocuments(OnDocuments);
	}
	return true;
}

bool UCrowdyConsentPromptSubsystem::OpenAgePrompt(bool bInPreview)
{
	UGameInstance* GameInstance = GetGameInstance();
	UGameViewportClient* ViewportClient = GameInstance ? GameInstance->GetGameViewportClient() : nullptr;
	if (!ViewportClient || !FSlateApplication::IsInitialized() || !FApp::CanEverRender())
	{
		return false;
	}

	AgePrompt = SNew(SCrowdyAgePrompt)
		.OnContinue(FSimpleDelegate::CreateUObject(this, &UCrowdyConsentPromptSubsystem::HandleAgeContinue))
		.OnDecline(FSimpleDelegate::CreateUObject(this, &UCrowdyConsentPromptSubsystem::HandleAgeDecline));
	ShowWidget(*ViewportClient, AgePrompt.ToSharedRef(), AgePrompt->GetInitialFocus());
	bPreview = bInPreview;
	return true;
}

void UCrowdyConsentPromptSubsystem::ShowWidget(UGameViewportClient& ViewportClient, const TSharedRef<SWidget>& Widget,
	const TSharedRef<SWidget>& Focus)
{
	ViewportClient.AddViewportWidgetContent(Widget, PromptZOrder);

	Shown = Widget;
	Viewport = &ViewportClient;
	bPreviousIgnoreInput = ViewportClient.IgnoreInput();
	PreviousCaptureMode = ViewportClient.GetMouseCaptureMode();
	PreviousLockMode = ViewportClient.GetMouseLockMode();
	ViewportClient.SetIgnoreInput(true);
	ViewportClient.SetMouseCaptureMode(EMouseCaptureMode::NoCapture);
	ViewportClient.SetMouseLockMode(EMouseLockMode::DoNotLock);

	// A game-only input mode has the pointer captured and locked; the prompt cannot be clicked until it is let go.
	FSlateApplication& SlateApp = FSlateApplication::Get();
	SlateApp.ReleaseAllPointerCapture();
	PreviousFocus = SlateApp.GetUserFocusedWidget(0);
	SlateApp.SetUserFocus(0, Focus, EFocusCause::SetDirectly);
}

void UCrowdyConsentPromptSubsystem::HandleAgeRequired()
{
	if (Shown.IsValid() && (bPreview || !Shown->GetParentWidget().IsValid()))
	{
		ClosePrompt();
	}
	if (Shown.IsValid())
	{
		return;
	}

	UCrowdyAuthentication* Authentication = GetAuthentication();
	if (!Authentication)
	{
		return;
	}

	// A refusal offers no way back to the date.
	if (Authentication->IsAgeEntryLocked())
	{
		Authentication->DeclineAgeEntry();
		return;
	}
	if (GetDefault<UCrowdySDKDeveloperSettings>()->bShowConsentPrompt && OpenAgePrompt(false))
	{
		return;
	}

	// With no UI here and no other listener to ask the player, the held sign-in would wait forever.
	const TArray<UObject*> Listeners = Authentication->OnAgeRequired.GetAllObjects();
	if (Listeners.Num() == 1 && Listeners[0] == this)
	{
		UE_LOG(LogCrowdyServices, Warning, TEXT("[CrowdyAuth] This process has no UI to ask the player's age; failing the held sign-in."));
		Authentication->DeclineAgeEntry();
	}
}

void UCrowdyConsentPromptSubsystem::HandleAgeContinue()
{
	if (bPreview)
	{
		ClosePrompt();
		return;
	}

	UCrowdyAuthentication* Authentication = GetAuthentication();
	if (!AgePrompt.IsValid() || !Authentication)
	{
		return;
	}

	AgePrompt->SetBusy(true);

	FOnPlayerAgeRecorded OnRecorded;
	OnRecorded.BindDynamic(this, &UCrowdyConsentPromptSubsystem::HandleConsentsRecorded);
	FOnAuthError OnError;
	OnError.BindDynamic(this, &UCrowdyConsentPromptSubsystem::HandleAgeRecordFailed);
	Authentication->RecordPlayerAge(AgePrompt->GetBirthYear(), AgePrompt->GetBirthMonth(), AgePrompt->GetCountryCode(),
		AgePrompt->HasParentalConfirmation(), OnRecorded, OnError);
}

void UCrowdyConsentPromptSubsystem::HandleAgeDecline()
{
	const bool bWasPreview = bPreview;
	ClosePrompt();
	if (bWasPreview)
	{
		return;
	}

	if (UCrowdyAuthentication* Authentication = GetAuthentication())
	{
		Authentication->DeclineAgeEntry();
	}
}

void UCrowdyConsentPromptSubsystem::HandleAgeRecordFailed(FString Message)
{
	if (!AgePrompt.IsValid())
	{
		return;
	}

	AgePrompt->SetBusy(false);
	const UCrowdyAuthentication* Authentication = GetAuthentication();
	if (Authentication && Authentication->IsAccessRefused())
	{
		AgePrompt->ShowRefusal(FText::FromString(Message));
		return;
	}
	AgePrompt->SetError(FText::FromString(Message));
}

void UCrowdyConsentPromptSubsystem::ShowAgePreview()
{
	if (Shown.IsValid() && !Shown->GetParentWidget().IsValid())
	{
		ClosePrompt();
	}
	if (!Shown.IsValid())
	{
		OpenAgePrompt(true);
	}
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
	Authentication->RecordPlayerConsents(true, false, OnRecorded, OnError);
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

void UCrowdyConsentPromptSubsystem::HandleLegalDocuments(const TArray<FCrowdyLegalDocument>& Documents)
{
	if (Prompt.IsValid())
	{
		Prompt->SetDocuments(Documents);
		Prompt->SetLoadingDocuments(false);
	}
}

void UCrowdyConsentPromptSubsystem::HandleAgeEntryEnded()
{
	// A refusal stays up until the player has read it.
	if (!AgePrompt.IsValid() || !AgePrompt->IsShowingRefusal())
	{
		ClosePrompt();
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
	if (!Shown.IsValid())
	{
		return;
	}

	UGameViewportClient* ViewportClient = Viewport.Get();
	if (ViewportClient)
	{
		ViewportClient->RemoveViewportWidgetContent(Shown.ToSharedRef());

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

	Shown.Reset();
	Prompt.Reset();
	AgePrompt.Reset();
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

static void PreviewAgePrompt(UWorld* World)
{
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (UCrowdyConsentPromptSubsystem* Host = GameInstance ? GameInstance->GetSubsystem<UCrowdyConsentPromptSubsystem>() : nullptr)
	{
		Host->ShowAgePreview();
	}
}

static FAutoConsoleCommandWithWorld GCrowdyAgePreviewCommand(
	TEXT("crowdy.auth.age.preview"),
	TEXT("Shows the age prompt over the game viewport without recording anything; its buttons only close it."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&PreviewAgePrompt));

static void UnlockAgeEntry(UWorld* World)
{
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	if (UCrowdyAuthentication* Authentication = GameInstance ? GameInstance->GetSubsystem<UCrowdyAuthentication>() : nullptr)
	{
		Authentication->ClearAgeEntryLock();
	}
}

static FAutoConsoleCommandWithWorld GCrowdyAgeUnlockCommand(
	TEXT("crowdy.auth.age.unlock"),
	TEXT("Lifts the age lock on this install, for testing a refusal."),
	FConsoleCommandWithWorldDelegate::CreateStatic(&UnlockAgeEntry));
#endif
