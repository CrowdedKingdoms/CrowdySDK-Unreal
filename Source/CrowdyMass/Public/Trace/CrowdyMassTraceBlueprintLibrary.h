#pragma once

#include "CoreMinimal.h"
#include "Fragments/CrowdyMassFragments.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Mass/EntityHandle.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "CrowdyMassTraceBlueprintLibrary.generated.h"

class AActor;
class UPrimitiveComponent;
class UCrowdyMassEntity;
class UCrowdyMassHitZoneTable;
class UWorld;
struct FMassRepresentationFragment;

namespace CrowdyMassTrace
{
	/**
	 * Whether a trace collision fragment (or its absence) lets the entity participate in trace
	 * collision at all. An entity with no fragment fails closed: it can never block a trace and never
	 * passes a trace filter, exactly as if it explicitly had bEnableTraceCollision = false. Exported
	 * as a pure function of the fragment pointer so the fail-closed contract can be asserted directly,
	 * without spinning up a world or a Mass entity manager.
	 */
	CROWDYMASS_API bool DoesTraceCollisionFragmentAllowParticipation(const FCrowdyMassTraceCollisionFragment* TraceCollisionFragment);

	/**
	 * The Crowdy Mass entities a world query actually reported, collected from that query's own results
	 * before the crowd is walked.
	 *
	 * This is the only thing that may suppress an entity's instanced row, and membership means REPORTED,
	 * never merely reportable. An entity's actor can be dropped from the world query for reasons the
	 * crowd pass knows nothing about: a class filter the pooled visual shell does not satisfy, an object
	 * type it does not answer, or the caller's ignore list. Suppressing the row of an entity the world
	 * query dropped makes that entity unhittable, and it is always one of the entities nearest the query,
	 * because those are the only ones given an actor at all.
	 *
	 * Small on purpose: it holds one entry per world result that resolved to a crowd entity, which is
	 * nothing at all on a query that never touched the crowd, so the storage stays on the stack and the
	 * per-entity test costs nothing when the query reported none.
	 */
	// Every member is inline, so this needs no export to be built or read from another module.
	struct FCrowdyMassWorldReportedEntities
	{
		/** Records one reported entity. An unset handle is a result that resolved to no entity, and adds nothing. */
		void Add(const FMassEntityHandle Entity)
		{
			if (Entity.IsSet())
			{
				Entities.AddUnique(Entity);
			}
		}

		bool Contains(const FMassEntityHandle Entity) const
		{
			// Empty on nearly every query, so the ordinary crowd walk never reaches the search.
			return !Entities.IsEmpty() && Entities.Contains(Entity);
		}

		bool IsEmpty() const { return Entities.IsEmpty(); }
		int32 Num() const { return Entities.Num(); }
		void Reset() { Entities.Reset(); }

	private:
		TArray<FMassEntityHandle, TInlineAllocator<8>> Entities;
	};

	/**
	 * Whether a Crowdy Mass entity should be tested as an instanced row by a trace.
	 *
	 * An entity the world trace already reported is skipped. The world trace runs first and
	 * unconditionally on every Crowdy Mass trace, and where it landed on an entity's own actor that hit
	 * is already the entity, named from the entity's own fragment. Testing the row as well would report
	 * one entity twice, and nothing downstream can merge the two, because a Mass hit is always
	 * synthesised as blocking and a prune to the first blocking hit keeps every entry before it.
	 *
	 * The question is what the world trace REPORTED, not what it could have reported. An entity holding
	 * an actor that the world query filtered out was never tested by anything, so its row is the only
	 * thing left that can be hit, and skipping it is what would make the entities closest to the query
	 * the only ones nothing can hit. The same is true through the whole of an asynchronous promotion:
	 * spawning an actor takes at least a frame, and a spawned actor is held disabled, with no collision,
	 * until it is ready to be seen, so nothing was reported and the row is still what is on screen.
	 *
	 * Exported as a pure function of the representation and one flag so both halves can be asserted
	 * directly, without a world, a renderer or a spawned actor.
	 */
	CROWDYMASS_API bool ShouldTraceEntityAsRow(const FMassRepresentationFragment& Representation, bool bReportedByWorldQuery);
}

USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassTraceHit
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	bool bBlockingHit = false;

	/**
	 * True when the hit landed on a Crowdy Mass entity, WHICHEVER WAY that entity is being drawn: an
	 * instanced row in the crowd, or the actor the entity has been given while it is close enough to need
	 * one. It does not mean "hit an instanced row"; InstanceIndex is what says that, and it stays
	 * INDEX_NONE for a hit on the actor.
	 *
	 * This is the flag to branch on when routing a hit, and the reason it covers both is that the two are
	 * one player. A hit on the actor leaving this false would send a caller down its ordinary-actor path
	 * looking for gameplay components on a pooled visual shell that has none, which is wrong routing
	 * rather than a hit that merely went missing.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	bool bHitMassEntity = false;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	bool bHitCrowdyEntity = false;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	FHitResult HitResult;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	TObjectPtr<AActor> Actor = nullptr;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	TObjectPtr<UPrimitiveComponent> Component = nullptr;

	/**
	 * Who was hit. For a hit on a Crowdy Mass entity this is read from the entity itself, never from the
	 * actor drawing it: the actors are pooled, so one is handed to the next entity that needs it and an id
	 * carried on the actor can name a player who is no longer standing there.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	FGuid UUID;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	FMassEntityHandle Entity;

	/**
	 * The same hit, addressed as a send target for UCrowdyMassEventLibrary::CrowdyMassCallEventOnEntity.
	 * Only set (FCrowdyMassEntityTarget::IsSet) when the hit actually landed on a Mass entity
	 * (bHitMassEntity); a hit on an ordinary actor leaves it unset, there is nothing here to address as
	 * one. Getting this does not mint an avatar; see GetEntity below for that.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	FCrowdyMassEntityTarget Target;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	FName BoneName = NAME_None;

	/** Named zone (for example "Head" or "Torso") the hit bone resolves to against the active UCrowdyMassHitZoneTable, or the table's default zone for an unmapped bone. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	FName HitZoneName = NAME_None;

	/**
	 * COSMETIC CLIENT-SIDE PREDICTION ONLY. This is the local hit zone table's multiplier, read
	 * straight off whichever client ran the trace. It is not gameplay data: the server holds the
	 * authoritative zone multiplier table and never reads this field, so no damage amount may ever be
	 * computed from it. Use it only for things that do not need to be correct, such as a hit-marker
	 * flourish or a local damage-number preview that the server's own result will supersede.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	float DamageMultiplier = 1.0f;

	/**
	 * Which instanced row was hit, or INDEX_NONE when there was no row: a hit on an ordinary actor, and
	 * also a hit on a Crowdy Mass entity through the actor it has been given, which has no instance.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Trace")
	int32 InstanceIndex = INDEX_NONE;

	void Reset()
	{
		bBlockingHit = false;
		bHitMassEntity = false;
		bHitCrowdyEntity = false;
		HitResult = FHitResult();
		Actor = nullptr;
		Component = nullptr;
		UUID.Invalidate();
		Entity.Reset();
		Target = FCrowdyMassEntityTarget();
		BoneName = NAME_None;
		HitZoneName = NAME_None;
		DamageMultiplier = 1.0f;
		InstanceIndex = INDEX_NONE;
	}

	void SetHitResult(const FHitResult& InHitResult)
	{
		HitResult = InHitResult;
		Actor = HitResult.GetActor();
		Component = HitResult.GetComponent();
		bBlockingHit = HitResult.bBlockingHit;
	}
};

namespace CrowdyMassTrace
{
	/**
	 * Names the Crowdy Mass entity a hit landed on, filling the entity half of the hit and leaving
	 * everything the world trace already wrote (the impact, the actor, the component, the bone) alone.
	 *
	 * The identity comes from the entity's own identity fragment and from nowhere else. What resolves a
	 * hit to a player downstream is the id alone, with nothing to cross-check it against, so an id taken
	 * from a pooled actor that has since been handed to another entity would land the result on the wrong
	 * player while looking entirely healthy.
	 *
	 * Fails closed: an entity that cannot be named is left as an ordinary hit rather than reported as a
	 * Crowdy Mass entity nobody can address. Returns whether the hit was named.
	 *
	 * The world is carried into the target so the hit can be acted on without re-resolving one; passing
	 * null simply leaves the target incomplete (FCrowdyMassEntityTarget::IsSet stays false), which is what
	 * lets this be exercised with no world at all.
	 */
	CROWDYMASS_API bool JoinEntityIdentityToTraceHit(FMassEntityHandle Entity, const FCrowdyRemoteIdentityFragment* IdentityFragment, UWorld* World, FCrowdyMassTraceHit& OutHit);

	/**
	 * Records what one finished world trace result reported, which is what the crowd pass then leaves
	 * alone.
	 *
	 * Reads the result AFTER it has been named, so what it records is what a caller will actually be
	 * handed. A result that landed on something that is not a Crowdy Mass entity, and a result on an
	 * entity's actor that could not be named from the entity's own fragment, both record nothing: neither
	 * reported an entity, so neither may suppress one.
	 */
	CROWDYMASS_API void RecordWorldReportedEntity(const FCrowdyMassTraceHit& WorldHit, FCrowdyMassWorldReportedEntities& OutReported);
}

USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassOverlapResult
{
	GENERATED_BODY()

	/**
	 * True when the overlap landed on a Crowdy Mass entity, whichever way that entity is being drawn: an
	 * instanced row in the crowd, or the actor the entity has been given while it is close enough to need
	 * one. Each overlapping entity is reported once, from whichever of the two found it.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Overlap")
	bool bOverlapMassEntity = false;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Overlap")
	bool bOverlapCrowdyEntity = false;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Overlap")
	TObjectPtr<AActor> Actor = nullptr;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Overlap")
	TObjectPtr<UPrimitiveComponent> Component = nullptr;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Overlap")
	FGuid UUID;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Overlap")
	FMassEntityHandle Entity;

	/**
	 * The same overlap, addressed as a target for everything in UCrowdyMassTargetLibrary and for
	 * UCrowdyMassEventLibrary::CrowdyMassCallEventOnEntity. Only set (FCrowdyMassEntityTarget::IsSet) when
	 * the overlap actually landed on a Mass entity (bOverlapMassEntity); an overlap with an ordinary actor
	 * leaves it unset, there is nothing here to address as one.
	 */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Overlap")
	FCrowdyMassEntityTarget Target;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Overlap")
	int32 InstanceIndex = INDEX_NONE;

	void Reset()
	{
		bOverlapMassEntity = false;
		bOverlapCrowdyEntity = false;
		Actor = nullptr;
		Component = nullptr;
		UUID.Invalidate();
		Entity.Reset();
		Target = FCrowdyMassEntityTarget();
		InstanceIndex = INDEX_NONE;
	}
};

namespace CrowdyMassTrace
{
	/**
	 * Records what one finished world overlap result reported, on the same rule as the trace: an entity
	 * is left to the world pass only where that pass actually named it.
	 *
	 * The overlap needs this more than the trace does. A world overlap is filtered by class, and the
	 * pooled actor a near entity is given is a visual shell that satisfies almost no gameplay class
	 * filter, so an overlap for a game's own pawn class routinely reports nothing for exactly the
	 * entities standing closest to the query.
	 */
	CROWDYMASS_API void RecordWorldReportedEntity(const FCrowdyMassOverlapResult& WorldOverlap, FCrowdyMassWorldReportedEntities& OutReported);
}

UCLASS()
class CROWDYMASS_API UCrowdyMassTraceBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Line Trace Single", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassLineTraceSingle(
		const UObject* WorldContextObject,
		FVector Start,
		FVector End,
		ETraceTypeQuery TraceChannel,
		bool bTraceComplex,
		const TArray<AActor*>& ActorsToIgnore,
		EDrawDebugTrace::Type DrawDebugType,
		FCrowdyMassTraceHit& OutHit,
		bool bIgnoreSelf = true,
		FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f),
		FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f),
		float DrawTime = 5.0f,
		bool bDrawMassCandidateShapes = false,
		bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Line Trace Multi", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassLineTraceMulti(
		const UObject* WorldContextObject,
		FVector Start,
		FVector End,
		ETraceTypeQuery TraceChannel,
		bool bTraceComplex,
		const TArray<AActor*>& ActorsToIgnore,
		EDrawDebugTrace::Type DrawDebugType,
		TArray<FCrowdyMassTraceHit>& OutHits,
		bool bIgnoreSelf = true,
		FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f),
		FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f),
		float DrawTime = 5.0f,
		bool bDrawMassCandidateShapes = false,
		bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Sphere Trace Single", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassSphereTraceSingle(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, ETraceTypeQuery TraceChannel, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Sphere Trace Multi", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassSphereTraceMulti(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, ETraceTypeQuery TraceChannel, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Box Trace Single", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassBoxTraceSingle(const UObject* WorldContextObject, FVector Start, FVector End, FVector HalfSize, FRotator Orientation, ETraceTypeQuery TraceChannel, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Box Trace Multi", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassBoxTraceMulti(const UObject* WorldContextObject, FVector Start, FVector End, FVector HalfSize, FRotator Orientation, ETraceTypeQuery TraceChannel, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Capsule Trace Single", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassCapsuleTraceSingle(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, float HalfHeight, ETraceTypeQuery TraceChannel, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Capsule Trace Multi", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassCapsuleTraceMulti(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, float HalfHeight, ETraceTypeQuery TraceChannel, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore,ObjectTypes", DisplayName="Crowdy Mass Line Trace Single For Objects", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassLineTraceSingleForObjects(const UObject* WorldContextObject, FVector Start, FVector End, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore,ObjectTypes", DisplayName="Crowdy Mass Line Trace Multi For Objects", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassLineTraceMultiForObjects(const UObject* WorldContextObject, FVector Start, FVector End, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore,ObjectTypes", DisplayName="Crowdy Mass Sphere Trace Single For Objects", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassSphereTraceSingleForObjects(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore,ObjectTypes", DisplayName="Crowdy Mass Sphere Trace Multi For Objects", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassSphereTraceMultiForObjects(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore,ObjectTypes", DisplayName="Crowdy Mass Box Trace Single For Objects", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassBoxTraceSingleForObjects(const UObject* WorldContextObject, FVector Start, FVector End, FVector HalfSize, FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore,ObjectTypes", DisplayName="Crowdy Mass Box Trace Multi For Objects", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassBoxTraceMultiForObjects(const UObject* WorldContextObject, FVector Start, FVector End, FVector HalfSize, FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore,ObjectTypes", DisplayName="Crowdy Mass Capsule Trace Single For Objects", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassCapsuleTraceSingleForObjects(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, float HalfHeight, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore,ObjectTypes", DisplayName="Crowdy Mass Capsule Trace Multi For Objects", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassCapsuleTraceMultiForObjects(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, float HalfHeight, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Line Trace Single By Profile", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassLineTraceSingleByProfile(const UObject* WorldContextObject, FVector Start, FVector End, FName ProfileName, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Line Trace Multi By Profile", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassLineTraceMultiByProfile(const UObject* WorldContextObject, FVector Start, FVector End, FName ProfileName, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Sphere Trace Single By Profile", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassSphereTraceSingleByProfile(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, FName ProfileName, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Sphere Trace Multi By Profile", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassSphereTraceMultiByProfile(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, FName ProfileName, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Box Trace Single By Profile", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassBoxTraceSingleByProfile(const UObject* WorldContextObject, FVector Start, FVector End, FVector HalfSize, FRotator Orientation, FName ProfileName, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Box Trace Multi By Profile", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassBoxTraceMultiByProfile(const UObject* WorldContextObject, FVector Start, FVector End, FVector HalfSize, FRotator Orientation, FName ProfileName, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Capsule Trace Single By Profile", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassCapsuleTraceSingleByProfile(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, float HalfHeight, FName ProfileName, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, FCrowdyMassTraceHit& OutHit, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ActorsToIgnore", DisplayName="Crowdy Mass Capsule Trace Multi By Profile", AdvancedDisplay="bIgnoreSelf,TraceColor,TraceHitColor,DrawTime,bDrawMassCandidateShapes,bDrawSpatialGridCells"))
	static bool CrowdyMassCapsuleTraceMultiByProfile(const UObject* WorldContextObject, FVector Start, FVector End, float Radius, float HalfHeight, FName ProfileName, bool bTraceComplex, const TArray<AActor*>& ActorsToIgnore, EDrawDebugTrace::Type DrawDebugType, TArray<FCrowdyMassTraceHit>& OutHits, bool bIgnoreSelf = true, FLinearColor TraceColor = FLinearColor(0.0f, 0.2f, 1.0f, 1.0f), FLinearColor TraceHitColor = FLinearColor(1.0f, 0.45f, 0.0f, 1.0f), float DrawTime = 5.0f, bool bDrawMassCandidateShapes = false, bool bDrawSpatialGridCells = false);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Overlap", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ObjectTypes,ActorsToIgnore", DisplayName="Crowdy Mass Sphere Overlap Actors"))
	static bool CrowdyMassSphereOverlapActors(const UObject* WorldContextObject, FVector SpherePos, float SphereRadius, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ActorClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Overlap", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ObjectTypes,ActorsToIgnore", DisplayName="Crowdy Mass Sphere Overlap Components"))
	static bool CrowdyMassSphereOverlapComponents(const UObject* WorldContextObject, FVector SpherePos, float SphereRadius, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ComponentClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Overlap", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ObjectTypes,ActorsToIgnore", DisplayName="Crowdy Mass Box Overlap Actors"))
	static bool CrowdyMassBoxOverlapActors(const UObject* WorldContextObject, FVector BoxPos, FVector BoxExtent, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ActorClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Overlap", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ObjectTypes,ActorsToIgnore", DisplayName="Crowdy Mass Box Overlap Actors With Orientation"))
	static bool CrowdyMassBoxOverlapActorsWithOrientation(const UObject* WorldContextObject, FVector BoxPos, FVector BoxExtent, FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ActorClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Overlap", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ObjectTypes,ActorsToIgnore", DisplayName="Crowdy Mass Box Overlap Components"))
	static bool CrowdyMassBoxOverlapComponents(const UObject* WorldContextObject, FVector BoxPos, FVector BoxExtent, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ComponentClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Overlap", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ObjectTypes,ActorsToIgnore", DisplayName="Crowdy Mass Box Overlap Components With Orientation"))
	static bool CrowdyMassBoxOverlapComponentsWithOrientation(const UObject* WorldContextObject, FVector BoxPos, FVector BoxExtent, FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ComponentClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Overlap", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ObjectTypes,ActorsToIgnore", DisplayName="Crowdy Mass Capsule Overlap Actors"))
	static bool CrowdyMassCapsuleOverlapActors(const UObject* WorldContextObject, FVector CapsulePos, float Radius, float HalfHeight, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ActorClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Overlap", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ObjectTypes,ActorsToIgnore", DisplayName="Crowdy Mass Capsule Overlap Actors With Orientation"))
	static bool CrowdyMassCapsuleOverlapActorsWithOrientation(const UObject* WorldContextObject, FVector CapsulePos, float Radius, float HalfHeight, FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ActorClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Overlap", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ObjectTypes,ActorsToIgnore", DisplayName="Crowdy Mass Capsule Overlap Components"))
	static bool CrowdyMassCapsuleOverlapComponents(const UObject* WorldContextObject, FVector CapsulePos, float Radius, float HalfHeight, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ComponentClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Overlap", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="ObjectTypes,ActorsToIgnore", DisplayName="Crowdy Mass Capsule Overlap Components With Orientation"))
	static bool CrowdyMassCapsuleOverlapComponentsWithOrientation(const UObject* WorldContextObject, FVector CapsulePos, float Radius, float HalfHeight, FRotator Orientation, const TArray<TEnumAsByte<EObjectTypeQuery>>& ObjectTypes, UClass* ComponentClassFilter, const TArray<AActor*>& ActorsToIgnore, TArray<FCrowdyMassOverlapResult>& OutOverlaps);

	/**
	 * Resolve a trace hit to the avatar object for the Mass entity it hit, minting the avatar on
	 * first use. Returns null for a hit that did not land on a tracked Crowdy Mass entity.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass|Trace", meta=(DisplayName="Get Entity"))
	static UCrowdyMassEntity* GetEntity(const FCrowdyMassTraceHit& Hit);

	/**
	 * C++ convenience wrapper: resolves the same avatar as GetEntity, then casts to T. Returns
	 * null instead of an invalid cast when the avatar isn't (or isn't derived from) T.
	 */
	template<typename T>
	static T* GetEntity(const FCrowdyMassTraceHit& Hit)
	{
		return Cast<T>(GetEntity(Hit));
	}

	/**
	 * Sets the hit zone table Crowdy Mass trace results are resolved against IN THIS WORLD. Pass null to
	 * go back to reporting every hit as the built-in default zone.
	 *
	 * CALL THIS ONCE, at startup. Every call walks the whole table into a flat per-bone lookup, which is
	 * what keeps the trace path free of any per-hit search; calling it from something per-pawn, such as a
	 * character's BeginPlay, rebuilds the same table once per pawn and turns a one-time cost into a
	 * per-spawn one for no change in the result.
	 *
	 * The table is world-scoped, so a second PIE session does not inherit the first one's assignment and
	 * nothing survives into the next play session.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", DisplayName="Set Crowdy Mass Hit Zone Table"))
	static void SetHitZoneTable(const UObject* WorldContextObject, UCrowdyMassHitZoneTable* HitZoneTable);

	/**
	 * Runtime switch for whether a Mass entity participates in Crowdy Mass trace collision: makes it
	 * traceable/blocking again, or takes it out of trace collision without removing the entity itself.
	 * Returns false if the entity is invalid or was never given a trace collision fragment in the
	 * first place (only entities built through the Crowdy Mass Trace Collision trait have one; there
	 * is nothing here to toggle for an entity that never opted in). Those two false answers are not the
	 * same thing, so the second one also names the missing trait in the log, once per world.
	 *
	 * Do not call this to make a dead entity untraceable. Entity lifetime, corpse windows and
	 * despawning belong to the tracker and rendering backend alone; a corpse must stay traceable for
	 * as long as it exists regardless of what this function has ever been called with.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Trace", meta=(WorldContext="WorldContextObject", DisplayName="Set Crowdy Mass Entity Trace Collision Enabled"))
	static bool SetEntityTraceCollisionEnabled(const UObject* WorldContextObject, FMassEntityHandle Entity, bool bEnabled);
};
