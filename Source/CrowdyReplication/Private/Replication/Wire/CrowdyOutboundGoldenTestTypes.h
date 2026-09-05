#pragma once

#include "CoreMinimal.h"
#include "CrowdyOutboundGoldenTestTypes.generated.h"

/**
 * A fixed plain-old-data actor state for the outbound golden vectors.
 *
 * Every member is a numeric leaf, so the bytes it serializes to are the packed field sequence in
 * declaration order. It is deliberately not shared with any other fixture: a golden vector pins the
 * exact bytes of this struct, so a field added here for some other test's benefit would move them.
 */
USTRUCT()
struct FCrowdyOutboundGoldenActorState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	float PositionX = 0.f;

	UPROPERTY()
	float PositionY = 0.f;

	UPROPERTY()
	float PositionZ = 0.f;

	UPROPERTY()
	uint8 Stance = 0;

	UPROPERTY()
	bool bAirborne = false;
};
