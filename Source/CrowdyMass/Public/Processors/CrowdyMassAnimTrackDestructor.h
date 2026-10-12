#pragma once

#include "CoreMinimal.h"
#include "MassEntityQuery.h"
#include "MassObserverProcessor.h"
#include "MassProcessor.h"
#include "Subsystems/CrowdyMassVisualReleaseQueue.h"
#include "CrowdyMassAnimTrackDestructor.generated.h"

/**
 * Records an entity's instanced-skinned-mesh visual when the entity is destroyed, so that the animation
 * track allocated for it can be released instead of being kept for the rest of the session.
 *
 * An entity drawn as an instanced skinned mesh is given an animation track the first time animation data
 * is batched for it, and that track is recorded against the entity in the mesh component's shared data.
 * The renderer has two ways of losing an instance and only one of them releases the track. The clean path
 * is the removal queue: an entity placed on it has its instance removed, its track deallocated and both
 * records erased. The other path is the sweep that runs during the batch update, which drops any instance
 * the entity did not ask to update this frame: it removes the instance and forgets its id, but never
 * touches the track. An entity that is simply destroyed takes the second path, so it disappears from the
 * screen while its track stays allocated forever. Tracks are drawn and skinned whether or not any
 * instance still points at them and the track pool never shrinks, so this accumulates as permanent GPU
 * cost, one track per entity that has ever existed.
 *
 * Only stationary entities reach the removal queue on their own, because everything that puts an entity
 * on it requires the static representation tag. This pass covers the movable entities that tag excludes.
 * It observes the removal of the representation fragment, which is what an entity's destruction looks
 * like, and records what UCrowdyMassVisualReleaseProcessor needs to hand the visual back on a later
 * frame.
 *
 * The release is deliberately NOT issued here. See UCrowdyMassVisualReleaseQueue::QueueVisualReleases for
 * why issuing it at the moment of destruction re-allocates the very track it was meant to free.
 *
 * Every entity that was batched as a skinned mesh instance is handled, whether or not it is still being
 * drawn as one. An entity close enough to be given its own actor stops being drawn as an instance, but
 * the sweep that drops that instance does not release its track either, so it is exactly as much of a
 * leak as a destroyed instance and is released the same way. What separates those from an entity that was
 * never drawn as an instance at all is the renderer's record of the track itself; the representation
 * fragment cannot answer it, because nothing on the fragment is written by batching.
 *
 * Static mesh instances have no animation track, and the sweep that drops their instances also erases the
 * record naming them, so nothing is left behind for those.
 *
 * The counters this keeps are reported through crowdy.mass.anim.dump. They describe what this pass
 * QUEUED, which is not the same as what was released: UCrowdyMassVisualReleaseProcessor reports that, and
 * the authoritative reading of whether the leak is closed is the live animation track count against the
 * live instance count that UCrowdyMassAnimationBatchAuditProcessor prints under the same variable.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassAnimTrackDestructor : public UMassObserverProcessor
{
	GENERATED_BODY()

public:
	UCrowdyMassAnimTrackDestructor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery EntityQuery;

	// Reused across chunks and across calls. Entities are destroyed in bursts of thousands, and filling a
	// flat local table lets the per-entity loop stay free of calls out to the subsystem: the queue is
	// handed the whole chunk at once.
	TArray<FCrowdyMassPendingVisualRelease> ChunkReleases;

	// Entities seen being destroyed, counted at the top of the chunk above every guard below, so that a
	// chunk this pass matched but could do nothing for is never mistaken for a chunk it never saw.
	int32 ObservedEntities = 0;

	// Of those, entities this pass could not even look at, counted apart from each other because they
	// send a reader to different places. No representation subsystem means the entity's archetype was
	// never given one. No mesh descriptions means the subsystem exists but nothing has registered a
	// skinned mesh description with it, which on a crowd that is visibly drawing is a configuration
	// fault, and which would otherwise be reported as this pass having matched nothing at all.
	int32 EntitiesWithNoRepresentationSubsystem = 0;
	int32 EntitiesWithNoMeshDescriptionTable = 0;

	// Entities whose visual was recorded for release, and entities whose record was refused because the
	// queue was full. The second is a leak and is warned about where it happens.
	int32 ReleasesQueued = 0;
	int32 ReleasesRefusedByFullQueue = 0;

	// Non-zero only while crowdy.mass.anim.releasefromobserver is on, which is a deliberately broken
	// mode kept so the animation track gate can be shown failing. Counted rather than left silent
	// because a leaking track count read while it is on is not a regression, it is the point.
	int32 ReleasesIssuedFromObserver = 0;

	// Entities that could not be matched to the batch that would be holding them, so if they hold an
	// animation track it stays allocated and nothing here can say whether they do. These are the residual
	// leak and are reported whether or not any debug console variable is on.
	int32 EntitiesWithUnknownMeshDescription = 0;
	int32 EntitiesWithNoSignificanceRange = 0;

	// Entities whose mesh description currently has no mesh components: either it was torn down, which
	// destroys every track recorded against it, or it has not been constructed yet, in which case no
	// track was ever allocated. Neither is a leak, so these are counted apart from the two above rather
	// than being reported as one.
	int32 EntitiesOnMeshDescriptionWithoutComponents = 0;

	// Entities holding no instanced skinned mesh visual to hand back: either never batched for one, or not
	// drawn as a skinned mesh at all. Counted apart because they would otherwise resolve to no mesh
	// description and no significance range, for a reason that is not a leak, and reporting them as one
	// sends a reader hunting for a track that was never allocated.
	//
	// Settled against the renderer's own record of which entities hold an animation track, plus the
	// entity still being drawn as a row, which is the case the record has not caught up with yet. Neither
	// the mesh description handle nor the previous LOD significance can settle it: an entity that spawns
	// already close enough to be given its own actor carries exactly the same pair as one that was drawn
	// as a row for a minute. Counting those as batched would empty this number out and take the reading
	// that separates "not a leak" from "residual leak" with it.
	int32 EntitiesNeverBatched = 0;

	// The previous LOD significance of the last entity that matched no range, so the warning can name the
	// value that failed to resolve rather than only how many did.
	float LastUnmatchedSignificance = 0.0f;

	// Wall-clock times the next line of each kind may be logged. Entities are commonly destroyed in
	// bursts of thousands, so without these a single despawn would print a line per batch.
	double NextSummaryTimeSeconds = 0.0;
	double NextUnknownDescriptionWarningTimeSeconds = 0.0;
	double NextNoRangeWarningTimeSeconds = 0.0;
};

/**
 * Hands back the instanced-skinned-mesh visuals of entities destroyed on an earlier frame, which is what
 * actually releases their animation tracks.
 *
 * This runs a frame behind UCrowdyMassAnimTrackDestructor on purpose, and the delay is the whole fix
 * rather than an implementation detail. The renderer processes one mesh component in a single pass: it
 * drains its removal queue first, erasing each entity's instance id and animation track, and then walks
 * the batch queue that this frame's processors filled and adds back anything it does not already have an
 * instance id for, allocating a fresh animation track as it goes. An entity that was batched earlier in
 * the same frame and then destroyed is therefore removed and immediately added again, with a new track
 * keyed to an entity that no longer exists. Nothing can ever release that track: this pass will not see
 * the entity again, and the sweep that later drops the orphaned instance does not touch tracks.
 *
 * Deferring by one frame removes that window entirely. On the frame after the entity died nothing batches
 * it, because it no longer exists and no query matches it, so the batch queue cannot contain it, the
 * removal step frees its instance and its track, and the sweep finds nothing left to do for it. The cost
 * is that a destroyed entity's instance stays on screen for one extra frame, which is exactly what
 * happened before as well.
 *
 * Running in the pre-physics phase and ahead of the representation group is what guarantees that
 * ordering: a release is always queued with the renderer before anything batches for the frame. This pass
 * and the passes that batch are all pinned to the game thread, so they cannot overlap.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassVisualReleaseProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UCrowdyMassVisualReleaseProcessor();

protected:
	// No ConfigureQueries override and no entity query: the work is a walk over records left by entities
	// that no longer exist, so there is nothing left to match them with.
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	// Reused every frame. Swapped with the subsystem's queue rather than copied out of it, so once the
	// crowd has settled at its usual burst size neither buffer reallocates.
	TArray<FCrowdyMassPendingVisualRelease> Releases;

	// Records taken off the queue, and of those, the ones actually handed to a live significance range.
	int32 ReleasesDrained = 0;
	int32 ReleasesIssued = 0;

	// Records whose world or whose mesh description had already been torn down by the time they were
	// drained. Not a leak: tearing either down destroys every animation track recorded against it.
	int32 ReleasesOnDepartedSubsystem = 0;
	int32 ReleasesOnDepartedDescription = 0;

	// Records whose LOD significance matched no range on a description that is still live. These are the
	// residual leak and are reported whether or not any debug console variable is on.
	int32 ReleasesWithNoSignificanceRange = 0;

	// The LOD significance of the last record that matched no range, so the warning can name the value
	// that failed to resolve rather than only how many did.
	float LastUnmatchedSignificance = 0.0f;

	// Said once per session, the first time anything is handed back, so that this pass working and this
	// pass never running can be told apart without turning any console variable on.
	bool bReportedFirstRelease = false;

	// Wall-clock times the next line of each kind may be logged.
	double NextSummaryTimeSeconds = 0.0;
	double NextNoRangeWarningTimeSeconds = 0.0;
};
