#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityHandle.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyMassVisualReleaseQueue.generated.h"

class UMassRepresentationSubsystem;

/**
 * How many destroyed entities' renderer visuals may wait to be handed back at once.
 *
 * The queue is drained every frame, so its natural depth is one frame's worth of destroyed entities and
 * a level flush of a large crowd is the realistic peak. The cap only engages where nothing is draining
 * the queue at all, which is a configuration fault rather than a load condition, and there the choice is
 * between a bounded number of leaked animation tracks and unbounded memory.
 */
inline constexpr int32 CrowdyMassMaxPendingVisualReleases = 65536;

/**
 * One destroyed entity's instanced-skinned-mesh visual, recorded so that it can be handed back to the
 * renderer on a later frame rather than at the moment the entity goes away.
 *
 * The entity handle names an entity that no longer exists by the time this is used. That is safe here
 * and only here: handing a visual back uses the handle purely as a key into the renderer's own
 * per-entity maps, and never resolves it against the entity manager.
 *
 * The mesh description is kept as an index rather than as a resolved range pointer, because the
 * renderer's description table can be rebuilt between the frame this is recorded and the frame it is
 * used. The index is re-resolved and re-validated at that point.
 */
struct FCrowdyMassPendingVisualRelease
{
	// Which representation subsystem holds the visual. Weak because the world it belongs to can be torn
	// down between the record being made and being drained.
	TWeakObjectPtr<UMassRepresentationSubsystem> RepresentationSubsystem;

	FMassEntityHandle Entity;

	// Index into that subsystem's instanced skinned mesh descriptions.
	int32 MeshDescriptionIndex = INDEX_NONE;

	// The LOD significance the entity was last batched at, which is what names the range currently
	// holding its instance.
	float LODSignificance = -1.0f;
};

/**
 * The per-world queue of destroyed entities' renderer visuals waiting to be handed back.
 *
 * This lives on a world subsystem that exists in every play world, not on the Mass backend's entity
 * subsystem, because a project's own Mass crowd can be drawn on a map whose backend is not Mass. It is per
 * world because an entity handle only means anything in one entity manager: two worlds exist at once
 * during level travel, and a process-wide queue would let one world's records name instances in the
 * other's renderer.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassVisualReleaseQueue : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	/**
	 * Records destroyed entities' instanced-skinned-mesh visuals for release on a later frame, and
	 * answers how many were recorded. A short count means the queue was full and the remainder were
	 * dropped, which leaks an animation track each.
	 *
	 * The release is deliberately not performed at the call site. The renderer drains its removal queue
	 * and its batch queue in one pass, in that order, and adds back anything left in the batch queue that
	 * the removal step just erased. Releasing an entity that was already batched earlier in the same
	 * frame therefore removes its instance and immediately adds it again with a fresh animation track,
	 * keyed to an entity that no longer exists, which nothing can ever release. Deferring to the next
	 * frame puts the release ahead of a batch queue that cannot contain the entity any more, because the
	 * entity is gone and nothing batches it.
	 */
	int32 QueueVisualReleases(TConstArrayView<FCrowdyMassPendingVisualRelease> Releases);

	// Hands the queued releases to the caller and leaves the queue empty. OutReleases is emptied first,
	// so anything it already held is discarded rather than merged.
	void DrainPendingVisualReleases(TArray<FCrowdyMassPendingVisualRelease>& OutReleases);

	// How many releases are waiting to be drained. A number that only ever grows means nothing is
	// draining them, which is otherwise indistinguishable from nothing being destroyed.
	int32 GetPendingVisualReleaseCount() const { return PendingVisualReleases.Num(); }

private:
	// Renderer visuals of destroyed entities, waiting to be handed back. Capped at
	// CrowdyMassMaxPendingVisualReleases so a world where nothing drains this grows a warning rather
	// than memory.
	TArray<FCrowdyMassPendingVisualRelease> PendingVisualReleases;

	// Wall-clock time the next "the visual release queue is full" warning may be logged. Entities are
	// destroyed in bursts of thousands, so a full queue would otherwise print a line per burst.
	double NextVisualReleaseOverflowWarningSeconds = 0.0;
};
