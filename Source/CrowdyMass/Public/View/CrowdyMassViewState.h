#pragma once

#include "CoreMinimal.h"

/**
 * The version byte a producer stamps on a view-state payload, and the value that byte holds when the
 * sender never wrote one.
 *
 * Nothing else about the payload is defined here. The FIELDS a payload carries are whatever a game
 * registers through UCrowdyMassEntitySubsystem::RegisterViewStateField, matched by name against
 * whatever wire struct that game sends; CrowdyMass itself registers only the version and the pose.
 * View state is not authority in any case: every byte of it comes from the observed player's own
 * client and is trivially forgeable, so nothing on the receiving side may treat it as more than a
 * hint about what to draw.
 */

// The current wire version of the view-state payload. Producers stamp this so a future layout change
// can tell an old payload from a new one; nothing branches on it yet.
inline constexpr uint8 CrowdyMassViewStateWireVersion = 1;

// What the version field holds when the sender never wrote one. This is the value a wire struct
// constructs the field with, so it is what a payload from a peer predating view state reads as: those
// bytes are simply absent from the payload and the field keeps its default. It has to be a value no
// producer ever stamps, which is why the first real version is 1 and not 0.
inline constexpr uint8 CrowdyMassViewStateWireVersionUnspecified = 0;
