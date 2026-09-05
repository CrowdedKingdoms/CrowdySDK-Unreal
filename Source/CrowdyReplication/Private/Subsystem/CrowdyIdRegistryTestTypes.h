#pragma once

#include "CoreMinimal.h"
#include "CrowdyIdRegistryTestTypes.generated.h"

/**
 * Fixtures for the wire-identifier automation tests.
 *
 * The numeric suffixes on the two probe types are load bearing: each name was
 * picked so that its path lands on a specific edge of the type-ID range, which
 * is what lets a test pin the range mapping instead of merely observing
 * whatever value falls out. Renaming either one defeats its test silently.
 */

// Its path hashes to an exact multiple of the range modulus, so the mapping has
// to lift it off the reserved zero value.
USTRUCT()
struct FCrowdyIdProbeLow7989
{
	GENERATED_BODY()
};

// Its path hashes to one below a multiple of the range modulus, the top of the
// representable range.
USTRUCT()
struct FCrowdyIdProbeHigh1372
{
	GENERATED_BODY()
};

// Distinct types used to force an ID collision by registering them under the
// same explicit ID, rather than waiting for two real payloads to collide.
USTRUCT()
struct FCrowdyIdConflictProbeA
{
	GENERATED_BODY()
};

USTRUCT()
struct FCrowdyIdConflictProbeB
{
	GENERATED_BODY()
};

USTRUCT()
struct FCrowdyIdConflictProbeC
{
	GENERATED_BODY()
};

/**
 * Stands in for an ordinary payload that has ended up holding the entity spawn event's wire ID.
 *
 * The width is load bearing. The spawn event's field-by-field framing writes about 160 octets, so a
 * narrower struct here would turn a decode into a write past the end of the allocation instead of a
 * failed assertion. Every word is a plain integer, so nothing in it is read as a pointer or as a
 * length whichever framing is applied.
 */
USTRUCT()
struct FCrowdyIdSpawnFramingProbe
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Words[48];
};

USTRUCT()
struct FCrowdyIdConflictProbeD
{
	GENERATED_BODY()
};
