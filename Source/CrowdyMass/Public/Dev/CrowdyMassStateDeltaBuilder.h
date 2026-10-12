#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

class UClass;
struct FCrowdyRepLayout;
struct FCrowdyStateDelta;

/**
 * One CrowdyState byte field a synthesized delta carries: a property name to write, resolved against
 * the target layout by name exactly as a real sender's own reflection walk would resolve it, and the
 * value to write there.
 */
struct FCrowdyMassStateDeltaByteField
{
	FName PropertyName;
	uint8 Value = 0;
};

/**
 * Builds a real FCrowdyStateDelta from a class, a layout and a set of named byte values, without any
 * game-specific type or a live entity to source it from. It exists so the CrowdyState receive path that
 * applies a delta to an entity with no UObject participant (see ICrowdyEntitySubscriber) can be
 * driven and gated from CrowdyMass alone.
 *
 * Every guard below refuses rather than best-effort-adjusts: a delta this builder let through with a
 * field silently dropped, or addressed to a class the caller's layout does not describe, would exercise
 * a shape no real sender could produce and the gate would be measuring nothing real.
 */
class CROWDYMASS_API FCrowdyMassStateDeltaBuilder
{
public:
	// Returns true and fills OutDelta on success. Returns false and fills OutWhy with a plain-words reason
	// on any refusal; OutDelta is left untouched in that case. OutWhy is for the caller to log or assert
	// against, not for this builder to log itself.
	static bool Build(const UClass& EntityClass, const FCrowdyRepLayout& Layout, const FGuid& EntityID,
		const FGuid& SenderID, TConstArrayView<FCrowdyMassStateDeltaByteField> Fields,
		FCrowdyStateDelta& OutDelta, FString& OutWhy);
};
