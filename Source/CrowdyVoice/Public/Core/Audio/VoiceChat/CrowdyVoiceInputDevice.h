#pragma once

#include "CoreMinimal.h"
#include "CrowdyVoiceInputDevice.generated.h"

/** A microphone voice chat can capture from. */
USTRUCT(BlueprintType, meta=(DisplayName="Crowdy Voice Input Device"))
struct CROWDYVOICE_API FCrowdyVoiceInputDevice
{
	GENERATED_BODY()

	/** The same for this microphone across sessions, so it can be saved and passed to Set Voice Input Device. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Voice")
	FString Id;

	/** The name the OS shows for it. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Voice")
	FString Name;

	/** Whether it is the OS default microphone right now. */
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Voice")
	bool bIsDefault = false;
};
