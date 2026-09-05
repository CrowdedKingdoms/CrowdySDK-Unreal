#pragma once

#include "CoreMinimal.h"
#include "CrowdyPodCopyPlanTestTypes.generated.h"

UENUM()
enum class ECrowdyPodPlanStance : uint8
{
	Standing,
	Crouched,
	Prone
};

/**
 * The shape a crowd sends: a transform, a couple of scalars and two small leaves. Every member is a
 * numeric leaf or a struct of numeric leaves, so the bytes it serializes to are the packed field
 * sequence and nothing else.
 */
USTRUCT()
struct FCrowdyPodPlanTransformState
{
	GENERATED_BODY()

	UPROPERTY()
	FVector Location = FVector::ZeroVector;

	UPROPERTY()
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY()
	float Speed = 0.f;

	UPROPERTY()
	float Health = 0.f;

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	bool bAirborne = false;

	UPROPERTY()
	uint8 Stance = 0;
};

/** A string leaf has no fixed width on the wire, so a struct carrying one has to be refused. */
USTRUCT()
struct FCrowdyPodPlanStringState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	FString Label;
};

/** An enum leaf reaches a persistent archive as its entry NAME, so its width follows the name. */
USTRUCT()
struct FCrowdyPodPlanEnumState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	ECrowdyPodPlanStance Stance = ECrowdyPodPlanStance::Standing;
};

UENUM()
enum ECrowdyPodPlanLegacyStance : int
{
	LegacyStance_Standing,
	LegacyStance_Crouched
};

/**
 * A TEnumAsByte member is an FByteProperty that names an enum, which is a different property class from
 * an enum class and reaches the plan builder down a different branch, so it needs its own refusal.
 */
USTRUCT()
struct FCrowdyPodPlanLegacyEnumState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	TEnumAsByte<ECrowdyPodPlanLegacyStance> LegacyStance = LegacyStance_Standing;
};

/** A dynamic array carries its own length, so its width is not its storage. */
USTRUCT()
struct FCrowdyPodPlanArrayState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	TArray<int32> Samples;
};

/**
 * A struct with no serializer of its own reaches a persistent archive as tagged properties, names and
 * widths and all, so a member of this type cannot be copied even though every leaf inside it could.
 */
USTRUCT()
struct FCrowdyPodPlanPlainInner
{
	GENERATED_BODY()

	UPROPERTY()
	int32 First = 0;

	UPROPERTY()
	int32 Second = 0;
};

USTRUCT()
struct FCrowdyPodPlanNestedPlainState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	FCrowdyPodPlanPlainInner Inner;
};

/**
 * A struct that serializes itself, and not as its own memory: it writes a version byte ahead of its
 * fields. Nothing about its reflected leaves says so, which is exactly why a struct that serializes
 * itself has to be probed through that serializer rather than reasoned about from its properties.
 */
USTRUCT()
struct FCrowdyPodPlanVersionedInner
{
	GENERATED_BODY()

	UPROPERTY()
	int32 First = 0;

	UPROPERTY()
	int32 Second = 0;

	bool Serialize(FArchive& Ar)
	{
		uint8 Version = 1;
		Ar << Version;
		Ar << First;
		Ar << Second;
		return true;
	}
};

template<>
struct TStructOpsTypeTraits<FCrowdyPodPlanVersionedInner>
	: public TStructOpsTypeTraitsBase2<FCrowdyPodPlanVersionedInner>
{
	enum { WithSerializer = true };
};

USTRUCT()
struct FCrowdyPodPlanVersionedState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	FCrowdyPodPlanVersionedInner Versioned;
};

/** A bitfield shares its byte with its neighbours, so no span of memory belongs to it alone. */
USTRUCT()
struct FCrowdyPodPlanBitfieldState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	uint8 bFlag : 1;

	FCrowdyPodPlanBitfieldState()
		: bFlag(0)
	{
	}
};

/** A fixed-size array serializes every element, so one element's width undercounts it. */
USTRUCT()
struct FCrowdyPodPlanStaticArrayState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY()
	int32 Samples[4];

	FCrowdyPodPlanStaticArrayState()
	{
		Samples[0] = 0;
		Samples[1] = 0;
		Samples[2] = 0;
		Samples[3] = 0;
	}
};

/** A transient member is skipped by a persistent archive, so copying it would invent bytes. */
USTRUCT()
struct FCrowdyPodPlanTransientState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Sequence = 0;

	UPROPERTY(Transient)
	int32 Scratch = 0;
};

/** A payload with no properties at all, which leaves the plan with no run to make. */
USTRUCT()
struct FCrowdyPodPlanEmptyState
{
	GENERATED_BODY()
};

/**
 * A flat two-field payload, registered in both wire registries so the actor-state framing and the event
 * framing can each be shown to take the plan.
 */
USTRUCT()
struct FCrowdyPodPlanFlatState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 A = 0;

	// Deliberately not zero, so an assertion that the field kept its default cannot be satisfied by
	// memory that merely happens to be zeroed.
	UPROPERTY()
	int32 B = 5150;
};
