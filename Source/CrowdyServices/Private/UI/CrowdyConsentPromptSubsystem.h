// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyConsentPromptSubsystem.generated.h"

class SCrowdyAgePrompt;
struct FCrowdyLegalDocument;
class SCrowdyConsentPrompt;
class SWidget;
class UCrowdyAuthentication;
class UGameViewportClient;

/**
 * Opens the branded consent prompt over the game viewport when sign-in is held for missing
 * consent, and closes it once nothing is held any more.
 */
UCLASS()
class UCrowdyConsentPromptSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	bool IsPromptOpen() const { return Shown.IsValid(); }

	UFUNCTION()
	void HandleLegalAcceptanceRequired();

	/** Opens the prompt to look at: Accept and Decline only close it, and nothing is recorded. */
	void ShowPreview();

	/** The same for the age prompt. */
	void ShowAgePreview();

	UFUNCTION()
	void HandleAgeRequired();

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for the game instance's authentication subsystem on an instance built outside a collection. */
	TWeakObjectPtr<UCrowdyAuthentication> AuthenticationForTest;
#endif

private:
	UCrowdyAuthentication* GetAuthentication() const;
	bool OpenPrompt(bool bInPreview);
	bool OpenAgePrompt(bool bInPreview);
	void ShowWidget(UGameViewportClient& ViewportClient, const TSharedRef<SWidget>& Widget, const TSharedRef<SWidget>& Focus);
	void HandleAgeContinue();
	void HandleAgeDecline();
	void HandleAccept();
	void HandleDecline();
	void ClosePrompt();

	UFUNCTION()
	void HandleLegalAcceptanceEnded();

	UFUNCTION()
	void HandleConsentsRecorded();

	UFUNCTION()
	void HandleConsentRecordFailed(FString Message);

	UFUNCTION()
	void HandleAgeRecordFailed(FString Message);

	UFUNCTION()
	void HandleAgeEntryEnded();

	UFUNCTION()
	void HandleLegalDocuments(const TArray<FCrowdyLegalDocument>& Documents);

	TSharedPtr<SCrowdyConsentPrompt> Prompt;
	TSharedPtr<SCrowdyAgePrompt> AgePrompt;
	TSharedPtr<SWidget> Shown;
	TWeakObjectPtr<UGameViewportClient> Viewport;
	TWeakPtr<SWidget> PreviousFocus;
	EMouseCaptureMode PreviousCaptureMode = EMouseCaptureMode::NoCapture;
	EMouseLockMode PreviousLockMode = EMouseLockMode::DoNotLock;
	bool bPreviousIgnoreInput = false;
	bool bPreview = false;
};
