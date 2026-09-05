#pragma once

#include "CoreMinimal.h"

/**
 * One server-owned attribute's value transition, captured at the point a pull result is diffed against the
 * cache of what this client last saw.
 *
 * Captured rather than broadcast immediately so a listener runs after the cache and every OnRep have
 * settled, never mid-diff: a handler that re-enters the model subsystem then sees a consistent cache.
 *
 * NewValueJson is empty for a removed key. Both values are compact JSON, so a caller that needs a typed
 * value parses one rather than reading a member off an object.
 */
struct FCrowdyAttributeChange
{
	FName Key;
	FString OldValueJson;
	FString NewValueJson;
};
