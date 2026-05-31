#pragma once

#include "CoreMinimal.h"

using FCrowdyTypeID = uint16;

static constexpr FCrowdyTypeID CROWDY_INVALID_TYPE_ID = 0;

enum class ECrowdyCategory: uint8
{
	Event,
	ActorUpdate
};