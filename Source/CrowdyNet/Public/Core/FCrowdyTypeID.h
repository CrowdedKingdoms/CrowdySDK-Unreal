#pragma once

#include "CoreMinimal.h"

using FCrowdyTypeID = uint16;

static constexpr FCrowdyTypeID CROWDY_INVALID_TYPE_ID = 0;

// Entity class identity carried on spawn events. Full 32 bits: class IDs
// never share a map with the 16-bit struct IDs.
using FCrowdyClassID = uint32;

static constexpr FCrowdyClassID CROWDY_INVALID_CLASS_ID = 0;

// An actor-state frame leads with this reserved type tag instead of a real one. Nothing else can
// produce it: FCrowdyTypeIDGenerator::GenerateFromStruct maps every struct into 1..65535, so a frame
// whose first tag is zero is always a framed one and a frame whose first tag is non-zero is always a
// sender predating the framing. That certainty is the point. It also makes the break safe in the other
// direction for free, since a build without this framing resolves the sentinel to no struct at all and
// already drops the frame rather than misreading it.
static constexpr FCrowdyTypeID CROWDY_ACTOR_STATE_SENTINEL = CROWDY_INVALID_TYPE_ID;

// Bumped whenever the actor-state framing is reshaped rather than merely appended to. A receiver
// refuses a version it does not know before reading any of the body, which is the one rejection this
// path is allowed to make: everything downstream tolerates a peer a version apart on purpose, and that
// tolerance cannot tell an appended field from a moved one.
static constexpr uint8 CROWDY_ACTOR_STATE_FORMAT_VERSION = 1;

enum class ECrowdyCategory: uint8
{
	Event,
	ActorUpdate
};