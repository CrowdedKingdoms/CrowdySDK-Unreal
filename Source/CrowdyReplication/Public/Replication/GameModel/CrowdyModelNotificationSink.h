// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * The minimal, carrier-agnostic hint a model-changed notification carries. Enough to decide WHAT to re-pull; it
 * carries no truth (pull, not push). Every carrier - the fallback spatial ping (opcode 138), the server-native
 * event (opcode 139), and the channel carrier (opcode 18) - normalizes to this one shape BEFORE the re-pull, so
 * a consumer handles all carriers identically. Keeping every carrier converged on one hint is what lets a new
 * carrier be added without touching any consumer.
 */
struct FCrowdyModelChangeHint
{
	// The changed entity's NetID when the carrier names it (the ping); invalid for a container-keyed carrier.
	FGuid EntityID;

	// The changed container's id when the carrier names it directly (the 139 state, the ping). Preferred over
	// EntityID when present, since each client may bind its own entity to a shared container.
	FString ContainerId;

	// The session the change belongs to, when carried; empty for app-global.
	FString SessionId;

	// The notification's event_type, for coarse filtering (ModelChangedEventType for a model-changed carrier).
	int32 EventType = 0;
};

/**
 * A source of "a model-changed notification arrived for container/entity X". Implemented by
 * UCrowdyGameModelSubsystem, which maps every inbound carrier onto ONE delegate. A consumer (the subsystem's own
 * re-pull today; a future UI or extracted module tomorrow) subscribes to OnModelChanged and never parses a
 * carrier itself. Plain C++ abstract class (every implementation and call site is native), matching
 * ICrowdyModelIdentity; the interface IS the seam, so honoring it keeps a later module extraction a file move.
 */
class ICrowdyModelNotificationSink
{
public:
	virtual ~ICrowdyModelNotificationSink() = default;

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnModelChanged, const FCrowdyModelChangeHint& /*Hint*/);

	// The delegate a consumer subscribes to; broadcast once per normalized notification, on the game thread.
	virtual FOnModelChanged& OnModelChanged() = 0;
};
