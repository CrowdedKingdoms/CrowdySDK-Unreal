#pragma once

#include "CoreMinimal.h"
#include "CrowdyChannelVoiceMode.generated.h"

/** How a player takes part in a channel's voice. */
UENUM(BlueprintType)
enum class ECrowdyChannelVoiceMode : uint8
{
	/** Hear the channel's members and send nothing to it, as an audience does. */
	Listen,

	/** Hear the channel's members and, while the microphone is open, talk to them. The player's role there needs Send Voice. */
	Talk
};
