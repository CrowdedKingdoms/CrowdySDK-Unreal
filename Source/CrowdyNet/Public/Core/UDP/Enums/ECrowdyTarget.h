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
	Everyone        = 0 UMETA(DisplayName = "Everyone", ToolTip = "Every client in range receives the event. The default."),
	Entity          = 1 UMETA(DisplayName = "Entity", ToolTip = "Only handlers on the entity named by TargetID (its entity NetID) run."),
	Owner           = 2 UMETA(DisplayName = "Owner", ToolTip = "Only the client named by TargetID (a player GUID) dispatches the event."),
	Host            = 3 UMETA(DisplayName = "Host", ToolTip = "Only the elected host dispatches the event."),
	AllExceptSender = 4 UMETA(DisplayName = "All Except Sender", ToolTip = "Every client in range except the one that sent the event."),
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
