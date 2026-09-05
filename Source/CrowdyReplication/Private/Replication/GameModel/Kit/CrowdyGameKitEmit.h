#pragma once

#include "CoreMinimal.h"
#include "CrowdyKitBridge.h"

class UCrowdyKitLayerPreset;

/**
 * Converts a list of authored kit layer presets into a deployable bundle. This is
 * the single place the reflection-side presets and the reflection-free bridge meet,
 * so it lives in a private header: it pulls CrowdyKitBridge.h (a private dependency),
 * and the public preset/config headers stay free of any bridge type.
 *
 * Null entries are skipped (an unconfigured layer slot). The returned bundle carries
 * the emit's success flag and, on failure, the reason (a validation error or a
 * cross-layer name collision).
 */
FCrowdyKitBundle CrowdyKitEmitFromPresets(const TArray<TObjectPtr<UCrowdyKitLayerPreset>>& Layers,
	int64 AppId, const FString& SessionId);
