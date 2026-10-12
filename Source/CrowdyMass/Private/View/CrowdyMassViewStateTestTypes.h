#pragma once

#include "CoreMinimal.h"
#include "CrowdyMassViewStateTestTypes.generated.h"

/**
 * Stands in for a consuming game's own wire struct, so CrowdyMass can exercise its decode path
 * without depending on any particular game's plugin.
 *
 * CrowdyMass claims two of these names and only two: Version and PoseId, registered through
 * UCrowdyMassEntitySubsystem::RegisterViewStateField. StanceFlags and HealthPercent are here because
 * a game's wire struct carries whatever fields that game wants, and nothing in CrowdyMass registers
 * an ingestor for either.
 *
 * That makes them a control for an unclaimed field, but only against a subsystem registered the way
 * these fixtures register one, with CrowdyMass's own fields and nothing else. A name is claimed
 * process-wide rather than per struct, so in a live world a game that registers either name does
 * receive these bytes. The control is a property of the fixture, not of the two field names.
 *
 * The transform is never asserted on. It is present only so that its byte length sits between the
 * type tag and the appended bytes exactly as a real wire struct's does.
 */
USTRUCT()
struct FCrowdyMassViewStateTestActorState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	// Zero, matching the real wire contract: this is the value the field holds when the sender never
	// wrote one, which is what a payload from a build predating view state decodes as.
	UPROPERTY()
	uint8 Version = 0;

	UPROPERTY()
	uint8 StanceFlags = 0;

	UPROPERTY()
	uint8 HealthPercent = 255;

	UPROPERTY()
	uint8 PoseId = 0;
};

/**
 * A wire struct carrying a claimed name at the wrong width: PoseId is a float here rather than a
 * single byte.
 *
 * A registered name is claimed across every wire struct in the process, including structs from games
 * this build has never seen, so matching on the name alone would read a float's low mantissa byte as
 * the pose. This struct is what makes the width check observable.
 */
USTRUCT()
struct FCrowdyMassViewStateWideFieldTestActorState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY()
	uint8 Version = 0;

	UPROPERTY()
	float PoseId = 0.0f;
};
