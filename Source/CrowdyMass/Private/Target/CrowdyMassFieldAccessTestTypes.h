#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityElementTypes.h"
#include "CrowdyMassFieldAccessTestTypes.generated.h"

/**
 * A single-byte enum, so the byte accessor's acceptance of an enum member is asserted against a real one
 * rather than only against a member declared as a raw byte.
 */
UENUM()
enum class ECrowdyMassFieldTestStance : uint8
{
	Standing,
	Crouched,
	Prone
};

/**
 * Stands in for a fragment a consuming game authors: one member of every type the field accessors expose,
 * plus one that must NOT be reachable.
 *
 * Declaration order here is the fixture's, not a performance one. A real fragment declares its widest
 * members first; this one is arranged so that a member of each kind sits at a different offset, which is
 * what makes a defect that resolves the wrong property show up as the wrong value rather than as a value
 * that happens to match.
 */
USTRUCT()
struct FCrowdyMassFieldTestFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Anchor = FVector::ZeroVector;

	UPROPERTY()
	int32 Charges = 0;

	UPROPERTY()
	float Heat = 0.0f;

	UPROPERTY()
	uint8 Stance = 0;

	UPROPERTY()
	ECrowdyMassFieldTestStance TypedStance = ECrowdyMassFieldTestStance::Standing;

	UPROPERTY()
	bool bAlerted = false;

	// Deliberately unmarked. A plain C++ member of a USTRUCT has no reflection entry at all, so it can
	// never be found by name, and registering it has to be refused rather than accepted and then silently
	// answering nothing forever.
	int32 Unmarked = 0;
};

/**
 * A second fragment carrying a member of the same name and type as the first, so a registration claiming a
 * name another binding already holds has somewhere else to point.
 */
USTRUCT()
struct FCrowdyMassFieldTestOtherFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Charges = 0;
};
