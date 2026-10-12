#pragma once

#include "CoreMinimal.h"
#include "CrowdyMassGateWireState.generated.h"

/**
 * The wire payload the gate harness builds its view-state echoes from.
 *
 * It stands in for a consuming game's own update struct so the harness can run without depending on
 * any one game's plugin. The field ORDER and the field NAMES are the whole point rather than a
 * convenience: the bytes are laid down in declared order, and ingestion finds each field by property
 * name through reflection, so a struct shaped like this exercises the live registration table exactly
 * as a game's own struct does. Renaming a field here does not fail to build, it silently stops that
 * field being ingested.
 *
 * Version is the only name this module registers an ingestor for. PoseId, StanceFlags and HealthPercent
 * are carried without one, so that a payload built here exercises whatever a consuming game has
 * registered as well as what this module has.
 *
 * A field name is claimed per world, not per struct: a registered name is matched by reflection
 * against every wire struct that reaches ingestion, including this one. So in a project whose game
 * registers ingestors for these names, echoes built from this struct DO land on that game's
 * fragments and can reach anything listening to them. That is deliberate, and it is why the gate that
 * pushes a hostile payload asserts only that nothing structural changed rather than asserting a field
 * went unread.
 *
 * The defaults are the fail-safe ones, chosen so that a payload which stops before these fields (an
 * older peer, or a cut-off frame) reads as an unstamped version and no pose rather than as something
 * a sender never said.
 */
USTRUCT()
struct FCrowdyMassGateWireState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	// Zero, not the current wire version: this is what the field holds when the sender never wrote one,
	// which is exactly what a payload from a build predating the echo decodes as.
	UPROPERTY()
	uint8 Version = 0;

	UPROPERTY()
	uint8 StanceFlags = 0;

	UPROPERTY()
	uint8 HealthPercent = 255;

	UPROPERTY()
	uint8 PoseId = 0;
};
