// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyModelChangedPing.generated.h"

/**
 * Fallback "model changed, re-pull" hint the acting client emits after a successful gameModelInvoke, so
 * peers that did not receive a server-native model-driven notification still re-pull the container's
 * authoritative state. It carries NO authoritative state itself the Game Model API stays the source of
 * truth; this is purely a nudge (pull, not push).
 *
 * Routed as an ordinary CrowdyEvent payload (an FInstancedStruct) through
 * UCrowdyEntitySubsystem::DispatchGameEvent NOT a pre-encoded RPC/channel blob handed to
 * PublishReliableRpc. Kept tiny (IDs only) so it also fits the reliable channel's 1024-byte cap if a later
 * phase routes it there.
 *
 * This is the graceful-degradation carrier. The PRIMARY path is the server-native SERVER_EVENT
 * notification declared on the function (see the SERVER_EVENT_NOTIFICATION receive path). Both carriers
 * funnel into the same UCrowdyGameModelSubsystem re-pull, so there is one apply path.
 */
USTRUCT()
struct FCrowdyModelChangedPing
{
	GENERATED_BODY()

	// The entity whose Game Model container changed (its CrowdyState NetID). Receivers map it to a bound
	// container and re-pull.
	UPROPERTY()
	FGuid EntityID;

	// The server container id, when the emitter knows it. Advisory receivers can resolve it from EntityID
	// via the subsystem's binding, but carrying it lets a receiver that has the binding skip a lookup.
	UPROPERTY()
	FString ContainerId;
};
