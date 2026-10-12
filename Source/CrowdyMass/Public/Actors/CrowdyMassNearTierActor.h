#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassActorPoolableInterface.h"
#include "CrowdyMassNearTierActor.generated.h"

class UCrowdyMassNearTierComponent;
class USkeletalMeshComponent;

namespace CrowdyMassNearTier
{
	/**
	 * Whether near tier promotion should say what it is doing.
	 *
	 * Nothing in the engine's actor pool logs, so whether an actor was built or reused is otherwise
	 * unobservable: a pool that never returns a hit and a pool that always does produce the same picture
	 * and the same frame time budget on paper. Turning this on is the only way to see which is happening.
	 */
	CROWDYMASS_API bool IsTraceEnabled();

	/**
	 * Whether an actor has a skinned mesh that has resolved an asset, and so has something to draw.
	 *
	 * Searched for rather than read off one particular component, because a shell is meant to be subclassed
	 * and a subclass commonly adds its own mesh rather than filling in an inherited one. Gating on a single
	 * pointer leaves such a subclass never ready, and never ready is not merely a delay: the crowd stops
	 * drawing the entity's row the moment a promotion is decided, so the entity is drawn by nothing at all
	 * until the engine gives up waiting and shows the actor anyway.
	 */
	CROWDYMASS_API bool HasDrawableSkinnedMesh(const AActor& Actor);

	/**
	 * The skeletal mesh an action should be played on, or null when the actor has nothing to play one on.
	 *
	 * Never simply the first skeletal mesh found: a mesh with no asset accepts an animation and draws
	 * nothing, and an actor's components have no authored order, so an actor carrying an empty mesh beside
	 * a real one would swallow actions on some runs and not others. A mesh with an asset is required, and
	 * the shell's own mesh wins among them.
	 */
	CROWDYMASS_API USkeletalMeshComponent* ChooseActionMesh(const AActor& Actor);
}

/**
 * The visual stand-in a crowd entity is promoted to when it comes close enough to be drawn as an actor
 * rather than as one row of a crowd.
 *
 * It is a picture and nothing else. The entity remains the thing that exists: it keeps its identity, its
 * networked state and its place in the crowd, and this actor is spawned, moved, pooled and released
 * around it as the camera moves. Several entities share one of these over a session, so it must hold no
 * state that belongs to the entity it currently stands for.
 *
 * What it deliberately does NOT carry:
 *
 * - No Crowdy entity component. That would register a second networked player for a character that
 *   already exists, and the crowd is precisely the representation of players who are elsewhere.
 * - No character movement. The entity's transform is written onto this actor every frame; a movement
 *   component would fight that write and win half of the time.
 * - No colliding capsule. Collision on the whole actor is switched on and off by the promotion itself,
 *   and a capsule would add a second body the crowd's own tracing does not know about.
 * - It must NOT implement the Crowdy event source interface. The entity's own Crowdy Mass entity object
 *   is the registered participant for that entity's id, and the SDK registry refuses a second live
 *   participant on one id; claiming the id here would break that entity's registration for the rest of
 *   its life.
 *
 * Subclass it in Blueprint to give it a mesh, an animation Blueprint and whatever decorates them.
 */
UCLASS(Blueprintable, meta=(DisplayName="Crowdy Mass Near Tier Actor"))
class CROWDYMASS_API ACrowdyMassNearTierActor : public AActor, public IMassActorPoolableInterface
{
	GENERATED_BODY()

public:
	ACrowdyMassNearTierActor();

	/**
	 * The pivot this actor is placed by. It carries nothing and draws nothing; the entity's transform is
	 * written onto it, and everything else hangs beneath it.
	 *
	 * It exists so the mesh below can be placed relative to the entity. A root component's own location
	 * and rotation cannot be edited, so a mesh used AS the root could never be turned to face the way its
	 * entity faces, which is the one adjustment nearly every skeletal mesh needs.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowdy Mass|Near Tier")
	TObjectPtr<USceneComponent> Root;

	/**
	 * What this actor draws, placed relative to the pivot above.
	 *
	 * Its relative transform is normally not authored by hand: while this actor stands for an entity, the
	 * transform the crowd draws that entity's mesh at is mirrored onto it, so a promoted character faces
	 * and stands exactly as its crowd row did. Authoring one here is for a shell that draws something the
	 * crowd has no opinion about.
	 *
	 * A subclass is free to add its own mesh instead of filling this one in. Readiness, hiding and action
	 * playback all answer on whatever the actor actually carries rather than on this pointer, and a mesh a
	 * subclass adds is that subclass's to place: only this one is mirrored.
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowdy Mass|Near Tier")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	/** Joins this actor to the entity it stands for, and drives it from that entity's fragments. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Crowdy Mass|Near Tier")
	TObjectPtr<UCrowdyMassNearTierComponent> NearTier;

	/**
	 * Where this actor's root belongs relative to the entity's own transform.
	 *
	 * It points FROM the entity TO this actor's root and is measured in the entity's own space, so it turns
	 * with the entity: the root ends up at the entity's location plus this offset rotated by the entity's
	 * rotation. A sign or a frame the wrong way round puts the actor at twice the offset from where it
	 * belongs and nothing about the picture says which of the two it was, so read that sentence before
	 * filling this in.
	 *
	 * Zero suits the shape this class ships with: the root sits at the entity's transform, the location the
	 * owner replicates (a Character's capsule centre), and the mesh takes the crowd's placement, which carries
	 * the drop to the feet. Set it only for a root that belongs somewhere other than that location.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Crowdy Mass|Near Tier")
	FVector EntityTransformOffset = FVector::ZeroVector;

	/**
	 * The offset above, asked for rather than read, so a shell that works it out from something can.
	 *
	 * Its direction and frame are described on the property. Nothing should apply it by hand: use
	 * MakeRootTransformFromEntity, which is the one place that knows how.
	 */
	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier")
	virtual FVector GetEntityTransformOffset() const;

	/**
	 * Where this actor's root belongs for an entity standing at EntityTransform.
	 *
	 * The only place the offset above is applied. Three sites need this answer, the spawn, the teleport and
	 * the per-frame follow, and an offset applied in a different frame or with a different sign at any one
	 * of them leaves the actor permanently displaced from its entity. The shipped default of zero makes
	 * every such disagreement invisible until a real shell sets an offset, which is exactly when it is
	 * hardest to attribute.
	 */
	FTransform MakeRootTransformFromEntity(const FTransform& EntityTransform) const;

	/**
	 * Whether this actor can stand in for its entity yet.
	 *
	 * The promotion asks this every frame while it holds off, and holding off is a HOLE rather than a
	 * cross-fade: a moving entity's crowd row stops being drawn the moment the promotion is decided, so
	 * every frame answered with false is a frame in which the entity is not drawn at all. Answer as soon
	 * as there is something to draw and no sooner, and never make this wait on anything slow.
	 */
	virtual bool IsReadyForRepresentation() const;

	/**
	 * Whether this actor's current appearance came out of the actor pool rather than from a fresh spawn,
	 * answered once and then forgotten.
	 *
	 * The pool tells an actor it is being reused and tells nobody else, so this is the only place that
	 * knows. It is consumed rather than read so that one appearance cannot be reported twice.
	 */
	bool ConsumeCameFromPool();

	virtual bool CanBePooled_Implementation() override;
	virtual void PrepareForPooling_Implementation() override;
	virtual void PrepareForGame_Implementation() override;

protected:
	virtual void BeginPlay() override;

private:
	/** Set by the pool when it hands this actor back out, cleared by the first thing that asks. */
	bool bCameFromPool = false;
};
