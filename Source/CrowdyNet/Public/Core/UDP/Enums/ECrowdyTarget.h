#pragma once

#include "CoreMinimal.h"
#include "ECrowdyTarget.generated.h"

/**
 * Addressing mode for a game event. Carried on the wire alongside TargetID and
 * consumed by UCrowdyEventRouter when routing the event to handlers.
 */
UENUM(BlueprintType)
enum class ECrowdyTarget : uint8
{
	Everyone        = 0 UMETA(DisplayName = "Everyone"),         // legacy broadcast — default, wire-compatible intent
	Entity          = 1 UMETA(DisplayName = "Entity"),           // TargetID = entity NetID
	Owner           = 2 UMETA(DisplayName = "Owner"),            // TargetID = player GUID; only that client dispatches
	Host            = 3 UMETA(DisplayName = "Host"),             // only the elected host dispatches
	AllExceptSender = 4 UMETA(DisplayName = "All Except Sender"),// everyone except the sending client
};

/**
 * User-facing scope for entity-addressed sends (SendCrowdyEvent /
 * UCrowdyEntityComponent::SendEvent): run the target entity's handlers on
 * every client, or only on the client that owns the entity.
 */
UENUM(BlueprintType)
enum class ECrowdyEventScope : uint8
{
	Everyone  UMETA(DisplayName = "Everyone"),
	OwnerOnly UMETA(DisplayName = "Owner Only"),
};
