#pragma once

#include "CoreMinimal.h"
#include "CrowdyBitwiseCompareTestTypes.generated.h"

/**
 * Fixtures for the bitwise-comparability predicate, one per case it has to separate. Their layouts are
 * the point, so nothing here should gain a member for another test's benefit: a field added to any of
 * them moves the very property the test reads.
 */

/** Accepted: two core math structs, each gap free, tiling 48 bytes exactly. */
USTRUCT()
struct FCrowdyBitwiseTransformState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;
};

/**
 * Accepted at 51 of its 56 bytes: the five trailing bytes belong to no property, and stopping short of them
 * is what makes the compare answer the same question a property walk answers. Deliberately the shape of the
 * state struct the game's own executor sends.
 */
USTRUCT()
struct FCrowdyBitwisePaddedState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY()
	uint8 Stance = 0;

	UPROPERTY()
	uint8 Gait = 0;

	UPROPERTY()
	uint8 Team = 0;
};

/**
 * Refused: seven bytes of alignment padding sit BETWEEN the two properties, and a compare cannot step over
 * a gap the way it can stop short of a tail.
 */
USTRUCT()
struct FCrowdyBitwiseGappedState
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Stance = 0;

	UPROPERTY()
	FVector Location = FVector::ZeroVector;
};

/** Refused: an FString compares by its characters and its bytes are a heap pointer. */
USTRUCT()
struct FCrowdyBitwiseTextState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FString Name;
};

/** Accepted: a native bool is a whole byte of its own, and the two properties tile the struct. */
USTRUCT()
struct FCrowdyBitwiseFlagsState
{
	GENERATED_BODY()

	UPROPERTY()
	bool bGrounded = false;

	UPROPERTY()
	uint8 Stance = 0;
};

/** Refused: a bitfield bool compares one masked bit of a byte it shares. */
USTRUCT()
struct FCrowdyBitwiseBitfieldState
{
	GENERATED_BODY()

	FCrowdyBitwiseBitfieldState()
		: bAirborne(0)
		, bCrouched(0)
	{
	}

	UPROPERTY()
	uint8 bAirborne : 1;

	UPROPERTY()
	uint8 bCrouched : 1;
};

/** Refused: FProperty::Identical compares element zero alone, so the other three bytes are unread. */
USTRUCT()
struct FCrowdyBitwiseArrayState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Samples[4] = {0, 0, 0, 0};
};
