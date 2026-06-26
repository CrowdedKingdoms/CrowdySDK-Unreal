// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "Logging/LogMacros.h"

// Module-wide log category for the top-level CrowdySDK module (plugin startup, the SDK subsystem,
// configuration). _API-exported so inline logging in public headers links across module boundaries.
CROWDYSDK_API DECLARE_LOG_CATEGORY_EXTERN(LogCrowdySDK, Log, All);

namespace CrowdySDKTrace
{
	// crowdy.sdk.trace — top-level SDK subsystem lifecycle, connection, and configuration load.
	CROWDYSDK_API bool Sdk();
}
