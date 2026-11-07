#pragma once

#include "CoreMinimal.h"
#include "Subsystem/CrowdySafety.h"
#include "UObject/Object.h"
#include "CrowdySafetyTestTypes.generated.h"

/** Records what the safety subsystem hands the delegates bound to it. */
UCLASS(Transient)
class UCrowdySafetyTestListener : public UObject
{
	GENERATED_BODY()

public:
	int32 Dones = 0;
	int32 SettingsAnswers = 0;
	FCrowdyCommunicationSettings Settings;
	TArray<FCrowdyPlayerContact> Contacts;
	FCrowdyPlayerContact Contact;
	TArray<ECrowdyConsentStatus> ConsentAnswers;
	int32 RecordingChanges = 0;
	TArray<FCrowdySafetyError> Errors;

	UFUNCTION()
	void HandleDone() { ++Dones; }

	UFUNCTION()
	void HandleError(const FCrowdySafetyError& Error) { Errors.Add(Error); }

	UFUNCTION()
	void HandleSettings(const FCrowdyCommunicationSettings& InSettings)
	{
		++SettingsAnswers;
		Settings = InSettings;
	}

	UFUNCTION()
	void HandleContacts(const TArray<FCrowdyPlayerContact>& InContacts) { Contacts = InContacts; }

	UFUNCTION()
	void HandleContact(const FCrowdyPlayerContact& InContact) { Contact = InContact; }

	UFUNCTION()
	void HandleConsent(ECrowdyConsentStatus Status) { ConsentAnswers.Add(Status); }

	UFUNCTION()
	void HandleRecording(const FCrowdyAppRecording& Recording) { ++RecordingChanges; }
};
