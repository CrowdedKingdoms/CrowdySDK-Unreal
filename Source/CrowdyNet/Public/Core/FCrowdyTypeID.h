#pragma once

#include "CoreMinimal.h"

using FCrowdyTypeID = uint16;

static constexpr FCrowdyTypeID CROWDY_INVALID_TYPE_ID = 0;

// Entity class identity carried on spawn events. Full 32 bits — class IDs
// never share a map with the 16-bit struct IDs.
using FCrowdyClassID = uint32;

static constexpr FCrowdyClassID CROWDY_INVALID_CLASS_ID = 0;

enum class ECrowdyCategory: uint8
{
	Event,
	ActorUpdate
};