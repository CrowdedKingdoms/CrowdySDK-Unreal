#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyEntityTypes.h"
#include "Engine/EngineTypes.h"
#include "Interpolation/CrowdyInterpolationTiming.h"
#include "Mass/EntityElementTypes.h"
#include "CrowdyMassFragments.generated.h"

USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassTransformState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass")
	FRotator Rotation = FRotator::ZeroRotator;

	/**
	 * How long the receiver should take to play this move, or a negative value to let it decide from
	 * the interval it actually observes between updates.
	 *
	 * Negative by default, because a fixed span here is right only while updates happen to arrive that
	 * far apart: name one and the receiver will play every move over it whatever the connection does,
	 * which is what makes an entity run ahead and then stand still. Set it only for a move whose pace
	 * is the point, such as zero for a teleport.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass", meta=(ClampMin="-1.0", ToolTip="Seconds the receiver should take to play this move. Negative lets the receiver time it from the interval it observes between updates, which is almost always what you want. Zero snaps."))
	float SmoothingDuration = -1.0f;
};

/**
 * Who this entity is on the network: the id every client knows it by, the client that owns it, and
 * this client's relationship to it.
 *
 * Owner and role are held here rather than looked up in the SDK's entity registry on demand, because
 * they are read on the send path of every event an entity originates and on every authority check, and
 * a registry lookup is a hash of a guid plus a scattered read. They are seeded when the entity is
 * created and are as current as the record was at that moment.
 *
 * Read for every entity on every frame, so the declaration order packs rather than reads well: widest
 * first, single-byte members last. The assertion below holds that shape, so a member added in the
 * wrong place breaks the build instead of quietly costing a cache line per entity.
 */
USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyRemoteIdentityFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass")
	FGuid UUID;

	// The client that owns this entity, as its player id. Invalid means no client owns it: it is a world
	// entity belonging to whichever client is currently the host.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass")
	FGuid OwnerID;

	// This client's relationship to the entity. HostOwned is the default because an entity that arrives
	// with no registry record is a world entity until something says otherwise, and the alternative
	// default (Owner) would let this client originate events on somebody else's character.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass")
	ECrowdyRole Role = ECrowdyRole::HostOwned;
};

// Two guids and one byte rounded up to the struct's four-byte alignment. Written in terms of the
// members rather than as a number so it stays right if a guid's own size ever moves.
static_assert(sizeof(FCrowdyRemoteIdentityFragment) == 2 * sizeof(FGuid) + 4,
	"FCrowdyRemoteIdentityFragment has gained padding. Declare wider members before narrower ones; this fragment is read for every entity every frame.");

USTRUCT()
struct CROWDYMASS_API FCrowdyRemoteInterpolationFragment : public FMassFragment
{
	GENERATED_BODY()

	FVector StartLocation = FVector::ZeroVector;
	FQuat StartRotation = FQuat::Identity;

	FVector TargetLocation = FVector::ZeroVector;
	FQuat TargetRotation = FQuat::Identity;

	int64 ServerTimestampMs = 0;
	float ElapsedSeconds = 0.0f;
	float SmoothingDuration = 0.1f;

	// Local arrival time of the previous update, on the same clock as FPlatformTime::Seconds. Zero
	// means nothing has arrived yet, so there is no interval to measure.
	double LastArrivalSeconds = 0.0;

	// What the intervals between recent arrivals have looked like. SmoothingDuration is derived from
	// these rather than from a setting, so a segment is replayed over roughly the time it took to
	// arrive instead of a fixed span it may overrun or undershoot.
	FCrowdyArrivalIntervalHistory ArrivalIntervals;

	// Turns the speed the entity appears to move at back into the speed it is really travelling at.
	// Replaying a segment over longer than it took to arrive makes the entity cross the ground more
	// slowly than the player it stands for did, and animation picks a gait from that speed.
	float VelocityScale = 1.0f;
};

USTRUCT()
struct CROWDYMASS_API FCrowdyRemoteInterpolatingTag : public FMassTag
{
	GENERATED_BODY()
};

USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassCollisionResponseOverride
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Collision")
	TEnumAsByte<ECollisionChannel> Channel = ECC_Visibility;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Collision")
	TEnumAsByte<ECollisionResponse> Response = ECR_Block;

	FCrowdyMassCollisionResponseOverride() = default;

	FCrowdyMassCollisionResponseOverride(ECollisionChannel InChannel, ECollisionResponse InResponse)
		: Channel(InChannel)
		, Response(InResponse)
	{
	}
};

USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassTraceCollisionSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Trace Collision")
	bool bEnableTraceCollision = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Trace Collision", meta=(EditCondition="bEnableTraceCollision"))
	TEnumAsByte<ECollisionChannel> ObjectType = ECC_WorldDynamic;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Trace Collision", meta=(EditCondition="bEnableTraceCollision"))
	TEnumAsByte<ECollisionResponse> DefaultResponse = ECR_Ignore;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Trace Collision", meta=(EditCondition="bEnableTraceCollision"))
	TArray<FCrowdyMassCollisionResponseOverride> ResponseOverrides;

	FCrowdyMassTraceCollisionSettings()
	{
		ResponseOverrides.Emplace(ECC_Camera, ECR_Block);
	}
};

/**
 * An entity with no fragment of this type at all is never traceable and never blocks anything: the
 * missing case fails closed. bEnableTraceCollision only ever governs an entity that already has one,
 * which the Crowdy Mass Trace Collision trait gives it defaulting to true; a stripped-down archetype
 * built without that trait is simply not part of trace collision, not "blocks everything by default".
 */
USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassTraceCollisionFragment : public FMassFragment
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Trace Collision")
	bool bEnableTraceCollision = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Trace Collision", meta=(EditCondition="bEnableTraceCollision"))
	TEnumAsByte<ECollisionChannel> ObjectType = ECC_WorldDynamic;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Trace Collision", meta=(EditCondition="bEnableTraceCollision"))
	FCollisionResponseContainer ResponseToChannels;

	FCrowdyMassTraceCollisionFragment()
		: ResponseToChannels(ECR_Block)
	{
	}
};
