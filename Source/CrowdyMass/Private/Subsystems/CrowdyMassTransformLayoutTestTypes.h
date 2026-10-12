#pragma once

#include "CoreMinimal.h"
#include "CrowdyMassTransformLayoutTestTypes.generated.h"

// Fixture state structs for the transform-layout diagnostic. Each one is used by exactly one case,
// because the layout verdict is memoised for the lifetime of the process: a struct shared between two
// cases would be reported in whichever ran first and silent in the other, and which one that is depends
// on the order the suite happens to run in.

// The spelling mistake the diagnostic exists for: real position and rotation data under names the
// transform read does not look for.
USTRUCT()
struct FCrowdyMassTransformLayoutMisnamedState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Pos = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rot = FRotator::ZeroRotator;
};

// A struct with no transform-like field of any kind, which is what a state payload carrying only game
// values looks like.
USTRUCT()
struct FCrowdyMassTransformLayoutValueOnlyState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Health = 0;
};

// Half of the pair: a rotation with nothing to place it, which yields no transform even though one of
// the two names is spelled exactly right.
USTRUCT()
struct FCrowdyMassTransformLayoutRotationOnlyState
{
	GENERATED_BODY()

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;
};

// Both names spelled right, one of them declared as a different vector type. The read matches on name
// AND type, so this yields no transform, and an author looking at it sees two correct-looking names.
USTRUCT()
struct FCrowdyMassTransformLayoutWrongTypeState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector3f Location = FVector3f::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;
};

// The accepted spellings, one struct each, so the diagnostic is pinned against staying silent on the
// shapes it is supposed to accept as well as speaking on the shapes it rejects.
USTRUCT()
struct FCrowdyMassTransformLayoutLocationState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY()
	float SmoothingDuration = 0.0f;
};

USTRUCT()
struct FCrowdyMassTransformLayoutPositionState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Position = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;
};

USTRUCT()
struct FCrowdyMassTransformLayoutTransformState
{
	GENERATED_BODY()

	UPROPERTY()
	FTransform Transform = FTransform::Identity;
};
