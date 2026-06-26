// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Logging/LogMacros.h"

// Module-wide log category for CrowdyVoice. Note: a few call sites already log under the engine's
// LogVoiceChat / LogVoiceService categories — those are intentionally left as-is. _API-exported so
// inline logging in public headers links across module boundaries.
CROWDYVOICE_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyVoice, Log, All);

namespace CrowdyVoiceTrace
{
	// crowdy.voice.trace — voice chat subsystem, capture/playback service, audio device monitoring.
	CROWDYVOICE_API bool Voice();
}
