#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Mass/EntityHandle.h"
#include "UObject/WeakObjectPtr.h"
#include "CrowdyMassNearTierComponent.generated.h"

class ACrowdyMassNearTierActor;
class FProperty;
class UAnimMontage;
class UCrowdyMassAnimSet;
class UCrowdyMassEntitySubsystem;
class UCrowdyMassServerRecordsSubsystem;
class USkeletalMeshComponent;
struct FCrowdyMassEntityTarget;
struct FMassEntityManager;
struct FSkinnedMeshInstanceVisualizationDesc;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdyMassNearTierServerValuesChanged, const TArray<FString>&, Changed);

namespace CrowdyMassNearTier
{
	/**
	 * Where an entity's mesh belongs relative to the entity itself, read off the description the crowd
	 * draws that entity through.
	 *
	 * The crowd does not draw a mesh at the entity's transform: it draws it at the mesh transform authored
	 * beside the mesh, applied to the entity's transform. A skeletal mesh almost always needs one, since
	 * the standard skeleton faces sideways until it is yawed. An actor standing in for that entity has to
	 * apply the same transform or it faces a different way from the row it replaced, which reads as the
	 * promotion turning the character rather than as a transform nobody carried across.
	 *
	 * Identity when the description names no mesh, which leaves whatever the shell authored standing.
	 */
	CROWDYMASS_API FTransform MeshTransformFromVisualizationDesc(const FSkinnedMeshInstanceVisualizationDesc& Desc);

	/**
	 * Where a shell's own mesh belongs: the crowd's placement for that entity, with whatever the shell's
	 * author placed the mesh at applied on top of it.
	 *
	 * COMPOSED rather than chosen between, because a description usually carries only part of what an
	 * actor needs. A character's is typically a drop to stand the mesh on the ground and no rotation at
	 * all, since the instanced path and a skeletal mesh component do not need the same turn to face the
	 * same way. Taking one and discarding the other loses whichever half the winner did not carry, and a
	 * mesh that is facing wrong or buried is the visible result.
	 *
	 * So what a shell authors on its mesh is a CORRECTION on top of the crowd's placement, not the whole
	 * placement: author only the difference. An unauthored shell composes with identity and takes the
	 * crowd's placement unchanged.
	 */
	CROWDYMASS_API FTransform ResolveShellMeshTransform(const FSkinnedMeshInstanceVisualizationDesc& Desc,
		const FTransform& Authored);

	/**
	 * How long an actor should blend into an action, given where in the clip it is starting.
	 *
	 * An action that starts at its beginning keeps the blend its owner authored, which is what that blend
	 * is for: the graph eases out of whatever it was showing and into the clip's first pose. An actor that
	 * picks up an action ALREADY RUNNING gets none, because its graph is in its default pose while the clip
	 * at that offset is mid-swing, and easing between the two shows a pose that neither representation of
	 * the character ever holds. That is visible as a flinch when an entity is promoted mid-action, and only
	 * then, since going the other way resumes a crowd row whose playback was never interrupted.
	 *
	 * The boundary is the authored blend length rather than zero, because a seek of exactly zero is not
	 * what an action that has just been triggered looks like from here: the simulation advances an action's
	 * elapsed time before anything ticking after it can read the fragment, so an ordinary action is already
	 * a frame or two old when it arrives, and testing against zero would take the blend off every action
	 * instead of off the ones that were already under way.
	 */
	CROWDYMASS_API float ActionBlendInForSeek(float AuthoredBlendInSeconds, float StartSecondsIntoAction);

	/**
	 * Whether a view should start the action its entity is holding.
	 *
	 * While an action runs its serial does not change, so ordinarily an action is started once and left to
	 * play. `bIsBeingShownNow` overrides that, and must: a mesh that is not being drawn does not tick, so a
	 * montage started when the view bound is still frozen at the frame it began on, and a mesh handed over
	 * by the actor pool is holding the pose of whoever it was drawing last. The serial cannot distinguish
	 * either case from a montage that is playing correctly, because in all three it has not changed.
	 *
	 * Freshness is by INEQUALITY and never by ordering. The serial wraps at the top of its range, and a
	 * "newer than" test loses every action for the rest of that entity's life the moment it does.
	 */
	CROWDYMASS_API bool ShouldStartHeldAction(bool bIsBeingShownNow, bool bHasStartedAnAction, int32 StartedSerial,
		int32 IncomingSerial);
}

/**
 * Makes an actor look like the crowd row it replaced.
 *
 * An entity close enough to the camera is drawn as a real actor instead of as one instance in a crowd,
 * and the two have to show the same thing: the same place, the same gait, the same pose, the same swing.
 * This component is the one-way view that keeps them agreeing. It READS the entity's Mass fragments and
 * writes nothing back to them, so the entity is still driven by exactly the processors that drive every
 * other entity and promoting one changes no state that anything else reads.
 *
 * What it drives:
 *  - the owning actor's transform, copied from the entity every frame,
 *  - one-shot actions, played into an animation slot as a dynamic montage,
 *  - trace collision settings, pushed onto the owner's primitive components so the entity's fragment
 *    stays the single source of truth for whether it can be hit,
 *  - a set of read-only animation inputs (speed, gait, pose, action) for the owner's animation graph.
 *
 * Everything else the graph needs it already has: the inputs below are plain values it can read from a
 * thread-safe update, so a game branches on a gait or a pose name without knowing this plugin exists.
 *
 * TICK GROUP IS LOAD-BEARING. Every processor that writes the fragments read here runs in the PrePhysics
 * Mass phase, and one of them runs off the game thread, so reading them during PrePhysics is a data race
 * rather than merely a frame-ordering question. This component ticks in PostPhysics and must keep doing
 * so.
 *
 * WHETHER it ticks belongs to the owner. A pooled shell is switched on when it starts standing for an
 * entity and off when it stops, so the component does no work for an actor sitting in the pool and cannot
 * keep moving one that has just been demoted. Nothing here carries over between ticks: every value is
 * re-read from the entity's fragments, so a tick disabled for any length of time and switched back on
 * resumes with the entity's current state and no catching up.
 */
UCLASS(ClassGroup=(CrowdyMass), meta=(BlueprintSpawnableComponent, DisplayName="Crowdy Mass Near Tier"))
class CROWDYMASS_API UCrowdyMassNearTierComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCrowdyMassNearTierComponent();

	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnUnregister() override;

	/**
	 * Binds this component to one entity, so the owning actor starts following it.
	 *
	 * Everything remembered about the previous occupant is dropped here, in the same step, because the
	 * actor outlives its occupants: it is pooled and handed a new entity later. A fresh entity's fragments
	 * are default-constructed, so its action serial starts again from zero, and a serial left over from
	 * the previous occupant would read as "already played" and eat the new entity's first action.
	 *
	 * False when Entity is unset or names nothing live, in which case the component is left unbound.
	 */
	bool BindToEntity(FMassEntityHandle Entity, const FGuid& UUID);

	/**
	 * Stops following whatever this component was bound to, and stops any action montage it started.
	 *
	 * Also takes back the collision this component pushed onto the owner, so an actor that has stopped
	 * standing for an entity stops answering traces as that entity. Without that, a shell released while
	 * its entity was still promoted sits in the pool invisible, at the dead entity's last transform, and
	 * still blocks every trace aimed through it.
	 */
	void UnbindFromEntity();

	/**
	 * Binds to whichever entity the owning actor was spawned for, if any.
	 *
	 * The engine records both directions of the actor-to-entity mapping when it spawns an actor for an
	 * entity, so an actor promoted by the representation system needs no explicit bind: this finds it.
	 * False when there is no such mapping, which is the ordinary answer for an actor nobody promoted.
	 */
	bool ResolveEntityFromOwner();

	/**
	 * Makes the owner's animation correct for the frame it starts being drawn on, instead of the frame
	 * after.
	 *
	 * Call this as the actor is shown. The promotion decides during the Mass phase at the head of a frame
	 * and unhides the actor there, while this component ticks after physics and a skeletal mesh evaluates
	 * its pose in a tick group earlier still, one that has already run by then. Enabling a tick partway
	 * through a frame does not make it tick that frame either. So the first frame a promoted character is
	 * on screen is drawn from whatever its mesh was already holding: a bind pose on a fresh actor, the
	 * previous occupant's pose on one out of the pool, and in both cases a graph whose speed, gait and pose
	 * inputs are still at zero. One frame of that is a visible flinch, which is what this removes.
	 *
	 * It re-reads every input, starts the action the entity is holding from where that action has actually
	 * got to, and evaluates the pose once with a delta of zero so nothing advances.
	 *
	 * Game thread only, and safe to call while unbound, in which case it does nothing.
	 */
	void PrimeAnimationForDisplay();

	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	bool IsBoundToEntity() const { return bBound; }

	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	FGuid GetEntityUUID() const { return EntityUUID; }

	FMassEntityHandle GetBoundEntity() const { return BoundEntity; }

	/**
	 * The entity's smoothed speed, which is the right locomotion input and the one the crowd uses.
	 *
	 * Not the length of the velocity below. A remote entity's velocity is produced by interpolating
	 * between position updates, so it reads the segment speed and then exactly zero until the next update
	 * lands: a sawtooth at the network update rate. Driving a blend space from that makes an actor stutter
	 * between walking and standing while the crowd behind it moves smoothly.
	 */
	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	float GetSmoothedSpeed() const { return SmoothedSpeed; }

	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	FVector GetWorldVelocity() const { return WorldVelocity; }

	// The same velocity in the owning actor's own space, which is what a strafing blend space wants.
	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	FVector GetLocalVelocity() const { return LocalVelocity; }

	/**
	 * The gait the crowd has selected for this entity, as a one-based row in its animation set.
	 *
	 * 0 means NO gait was resolved, which is a real answer and not an error: an entity whose animation set
	 * did not resolve, or one whose set authors no gait rows at all, holds 0 for its whole life. It is
	 * reported honestly rather than nudged to row 1, because a graph that treats "no gait" as "the slowest
	 * gait" shows a confident wrong answer instead of nothing.
	 */
	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	int32 GetGaitId() const { return GaitId; }

	// The authored name of the gait above, or None when no gait is resolved. Resolved only when the id
	// changes, so branching on the name costs a graph no more than branching on the id.
	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	FName GetGaitName() const { return GaitName; }

	// A one-based row in the entity's animation set, or 0 for no pose.
	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	int32 GetPoseId() const { return PoseId; }

	// The authored name of the pose above, or None when there is no pose. An id is a row position and
	// means nothing across classes; the name is what lets a graph branch without casting to a game class.
	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	FName GetPoseName() const { return PoseName; }

	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	bool HasPose() const { return PoseId != 0; }

	// 0 while no action is active.
	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	int32 GetActionId() const { return ActionId; }

	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	int32 GetActionVariant() const { return ActionVariant; }

	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	float GetActionPlayRate() const { return ActionPlayRate; }

	// Incremented by every trigger, including one that repeats the same action id. Exposed so a graph can
	// react to a retrigger of an action it is already showing.
	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	int32 GetActionSerial() const { return ActionSerial; }

	UFUNCTION(BlueprintPure, Category="Crowdy Mass|Near Tier", meta=(BlueprintThreadSafe))
	float GetActionElapsedSeconds() const { return ActionElapsedSeconds; }

	// The animation set this component resolved for the bound entity, or null when nothing names one.
	const UCrowdyMassAnimSet* GetResolvedAnimSet() const { return ResolvedAnimSet.Get(); }

	/**
	 * Whether actions are being played at all.
	 *
	 * False when the owning actor's own class names a different animation set from the one the entity
	 * renders through. An action id is a row position in one set, so the two sets disagreeing means
	 * nothing here can tell which clip the id names, and a wrong clip is worse than no clip.
	 */
	bool AreActionsPlayable() const { return !bActionsRefused; }

	// The slot node the owner's animation graph exposes for one-shot actions. The engine's default graph
	// slot is DefaultSlot; a graph with no slot of this name plays nothing and says so once.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Near Tier")
	FName ActionSlotName = TEXT("DefaultSlot");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Near Tier", meta=(ClampMin="0.0"))
	float ActionBlendInSeconds = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Near Tier", meta=(ClampMin="0.0"))
	float ActionBlendOutSeconds = 0.1f;

	/**
	 * Whether this component moves the owning actor to the entity's transform every frame.
	 *
	 * Leave it on. Nothing else moves an actor promoted out of a crowd: the engine places it once when it
	 * is promoted and never touches it again, so turning this off leaves the actor standing where it was
	 * born while the entity it stands for walks away. Turn it off only for an owner that drives its own
	 * movement from somewhere else and just wants the animation inputs.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Near Tier")
	bool bDriveOwnerTransform = true;

	/**
	 * Whether the entity's trace collision fragment decides the owner's collision object type and channel
	 * responses.
	 *
	 * On, the fragment is the single source of truth for both representations, so switching an entity's
	 * trace collision off reaches it whether it is currently drawn as a row or as an actor. Off, the
	 * owner keeps whatever its own class authored and the two can disagree.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy Mass|Near Tier")
	bool bApplyTraceCollision = true;

	/** After each change of the bound entity's server values, naming the variables; empty when its record was removed. */
	UPROPERTY(BlueprintAssignable, Category="Crowdy Mass|Server Values")
	FCrowdyMassNearTierServerValuesChanged OnServerValuesChanged;

	/** Reads the bound entity's server variable called Name into Value, as Get Server Value on its entity target does. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category="Crowdy Mass|Server Values", meta=(CustomStructureParam="Value", DisplayName="Get Server Value"))
	bool GetServerValue(FName Name, int32& Value);

	/** Get Server Value for C++: Dest is of type DestProperty. */
	bool ReadServerValueInto(FName Name, const FProperty* DestProperty, void* Dest) const;

private:
	friend struct FCrowdyMassServerRecordsTestAccess;

	DECLARE_FUNCTION(execGetServerValue);

	UCrowdyMassServerRecordsSubsystem* GetServerRecords() const;
	void WatchServerValues();
	void UnwatchServerValues();
	void HandleServerValuesChanged(const FCrowdyMassEntityTarget& Target, TConstArrayView<FString> Changed);

	FMassEntityHandle ServerValuesEntity;
	FDelegateHandle ServerValuesHandle;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakObjectPtr<UCrowdyMassServerRecordsSubsystem> ServerRecordsForTest;
#endif

	// Sets the bound entity and clears every value cached about the previous one, which must happen
	// together: see BindToEntity.
	void SetBoundEntity(FMassEntityHandle Entity, const FGuid& UUID);

	void ClearAnimationInputs();

	UCrowdyMassEntitySubsystem* GetEntitySubsystem() const;
	FMassEntityManager* GetEntityManager() const;

	// Works out which animation set the ENTITY renders through, and whether the owner's own class would
	// resolve a different one. Called once per bind, never per frame.
	void ResolveAnimSetForBoundEntity();

	// Reads everything the owner's animation graph is driven from off the bound entity, and starts the
	// action it is holding when there is one to start. Shared by the per-frame tick and the moment the
	// actor starts being drawn, which differ only in bIsBeingShownNow: see ShouldStartHeldAction for what
	// that changes and why the serial alone cannot decide it.
	void UpdateAnimationStateFromEntity(const FMassEntityManager& EntityManager, bool bIsBeingShownNow);

	// Evaluates the owner's pose immediately rather than waiting for the mesh's own tick. Costs one
	// skeletal evaluation, once per promotion.
	void ForceCurrentPose();

	void ApplyTransformFromEntity(const FTransform& EntityTransform);

	// Reads the bound entity's trace collision fragment and pushes it onto the owner. Called on bind as
	// well as per tick, because the entity's crowd row stops being traced the moment the actor is enabled
	// and the actor has to be answering as that entity on the same frame.
	void ApplyTraceCollisionFromBoundEntity(const FMassEntityManager& EntityManager);

	// Places the shell's own mesh where the crowd draws this entity's mesh, so promotion changes the
	// fidelity of a character and not the way it faces. Called once per bind.
	void ApplyMeshTransformFromBoundEntity(const FMassEntityManager& EntityManager);

public:
	/**
	 * Where the shell's author placed its mesh, remembered from before anything here moved it.
	 *
	 * Public because it is the one piece of this that can be asked without a world, and it is worth
	 * asking: reading the mesh's CURRENT transform instead would compose the crowd's placement into a
	 * value that already contains it, once per promotion, and a pooled actor would sink by that offset
	 * every time it crossed the boundary until it vanished into the ground.
	 */
	const FTransform& CaptureAuthoredMeshTransform(const USkeletalMeshComponent& InMesh);

private:

	void ApplyTraceCollisionFromFragment(bool bEnable, ECollisionChannel ObjectType, const FCollisionResponseContainer& Responses);

	// Takes the query collision back off whatever this component last pushed it onto, leaving the physics
	// and probe halves as they were found. The exact inverse of the push above.
	void ClearAppliedTraceCollision();

	void PlayActionFromFragment(int32 InActionId, float InPlayRate, float ElapsedSecondsSinceTrigger, float DurationSeconds);
	void StopActiveActionMontage();

	FMassEntityHandle BoundEntity;
	FGuid EntityUUID;
	bool bBound = false;

	// True once the owner's class and the entity's class were found to name different animation sets, so
	// no action id can be resolved with confidence. Motion and pose inputs keep working.
	bool bActionsRefused = false;

	float SmoothedSpeed = 0.0f;
	FVector WorldVelocity = FVector::ZeroVector;
	FVector LocalVelocity = FVector::ZeroVector;

	int32 GaitId = 0;
	FName GaitName;
	int32 PoseId = 0;
	FName PoseName;

	int32 ActionId = 0;
	int32 ActionVariant = 0;
	float ActionPlayRate = 1.0f;
	int32 ActionSerial = 0;
	float ActionElapsedSeconds = 0.0f;

	// The serial whose montage this component has already started. Compared by INEQUALITY, never by
	// ordering: the serial wraps at the top of its range, so a "newer than" test loses every action for
	// the rest of the entity's life once it does.
	int32 StartedActionSerial = 0;
	bool bHasStartedActionSerial = false;

	// The last gait and pose ids a name was resolved for, so a name lookup happens on a change and not
	// on every frame.
	int32 NamedGaitId = 0;
	int32 NamedPoseId = 0;

	bool bHasAppliedTraceCollision = false;

	// The mesh's placement as its author left it, and whether it has been taken yet. NOT cleared when the
	// entity changes: it describes the actor, not the entity currently borrowing it, and re-taking it after
	// this component has moved the mesh would read back its own previous answer.
	FTransform AuthoredMeshTransform;
	bool bHasAuthoredMeshTransform = false;
	bool bAppliedTraceCollisionEnabled = false;
	TEnumAsByte<ECollisionChannel> AppliedTraceObjectType = ECC_WorldDynamic;
	FCollisionResponseContainer AppliedTraceResponses;

	TWeakObjectPtr<const UCrowdyMassAnimSet> ResolvedAnimSet;
	TWeakObjectPtr<USkeletalMeshComponent> ActionMesh;
	TWeakObjectPtr<UAnimMontage> ActiveActionMontage;

	// The owner as the pooled shell class, when it is one, so the per-frame transform copy can ask it for
	// the root transform an entity transform means without casting every frame. Null for any other owner,
	// whose transform is then the entity's own.
	TWeakObjectPtr<ACrowdyMassNearTierActor> ShellOwner;
};
