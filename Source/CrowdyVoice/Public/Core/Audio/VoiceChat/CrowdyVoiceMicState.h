#pragma once

#include "CoreMinimal.h"
#include "CrowdyVoiceMicState.generated.h"

/** What the local microphone is doing for voice chat. */
UENUM(BlueprintType, meta=(DisplayName="Crowdy Voice Mic State"))
enum class ECrowdyVoiceMicState : uint8
{
	Off UMETA(DisplayName="Off", ToolTip="Voice chat is not using the microphone."),
	Starting UMETA(DisplayName="Starting", ToolTip="The microphone is open and its first audio has not arrived yet."),
	Capturing UMETA(DisplayName="Capturing", ToolTip="The microphone is live. Your voice is sent while you speak; silence is not sent."),
	NoDevice UMETA(DisplayName="No Device", ToolTip="No microphone was found. Voice chat opens one as soon as it is available."),
	Error UMETA(DisplayName="Error", ToolTip="The microphone could not be used, or this platform cannot capture audio. Check that it is connected and that the OS allows this app to use it, then start voice chat again.")
};
