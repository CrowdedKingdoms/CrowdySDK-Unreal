// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyEffectNotificationCarrier.generated.h"

/**
 * The reflected (details-panel / config) choice of which realtime carrier an authored effect's model-driven
 * "model changed" notification uses, so peers re-pull after a server-authoritative change. This is the editor
 * surface that resolves down to the pure lowering enum ECrowdyModelNotificationCarrier (CrowdyEffectLowering.h):
 * an effect asset picks Default to defer to the project setting, or pins a concrete carrier. The project setting
 * (UCrowdySDKDeveloperSettings::DefaultModelNotificationCarrier) supplies the fallback; a project-level Default
 * resolves to Channel, the SDK's position-independent default (see UCrowdyEffect::ResolveCarrier).
 */
UENUM(BlueprintType)
enum class ECrowdyEffectNotificationCarrier : uint8
{
	// The effect defers to the project's DefaultModelNotificationCarrier (which itself defaults to Channel). The
	// only sensible value for an effect asset that has no reason to override the project policy.
	Default UMETA(DisplayName = "Default (project setting)"),

	// Emit no model-driven notification. The acting client's confirmed-mutation apply still fires its own OnRep;
	// peers only converge on their next pull. Use for a purely local effect or when a different carrier drives the
	// re-pull.
	None,

	// Reach every member of the app's default session channel (__crowdy_session_<appId>) regardless of position.
	// The position-independent default: correct for turn-based and session-scoped games and for free/data
	// containers that carry no chunk coordinates.
	Channel,

	// Fan out by proximity over the server-event (opcode 139) path, for a location-bound change on a container that
	// carries chunk coordinates. A container with no position cannot target spatially; use Channel.
	Spatial,
};
