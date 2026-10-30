#pragma once

#include "Logging/LogMacros.h"

// Exported so inline logging in public headers links across module boundaries.
CROWDYVOICE_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyVoice, Log, All);

namespace CrowdyVoiceTrace
{
	// crowdy.voice.trace: microphone, playback and audio device activity.
	CROWDYVOICE_API bool Voice();
}
