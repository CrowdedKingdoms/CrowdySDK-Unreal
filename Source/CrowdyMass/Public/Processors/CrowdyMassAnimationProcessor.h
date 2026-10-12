#pragma once

#include "Assets/CrowdyMassAnimSet.h"
#include "CoreMinimal.h"
#include "MassEntityQuery.h"
#include "MassProcessor.h"
#include "UObject/ObjectKey.h"
#include "CrowdyMassAnimationProcessor.generated.h"

struct FMassInstancedSkinnedMeshInfo;

/**
 * One anim set paired with one animation bank. Both halves belong in the key: the same anim set can
 * be shared by archetypes that render through different banks, in which case the same authored
 * sequence sits at a different index in each, and the same bank can be read by more than one anim
 * set.
 */
struct FCrowdyMassAnimSetBankKey
{
	FObjectKey AnimSet;
	FObjectKey Bank;

	bool operator==(const FCrowdyMassAnimSetBankKey& Other) const
	{
		return AnimSet == Other.AnimSet && Bank == Other.Bank;
	}

	friend uint32 GetTypeHash(const FCrowdyMassAnimSetBankKey& Key)
	{
		return HashCombine(GetTypeHash(Key.AnimSet), GetTypeHash(Key.Bank));
	}
};

/**
 * Chooses each entity's animation from how it is moving and what state it is in, and writes the
 * result into the animation data the instanced-skinned-mesh batching reads off the entity.
 *
 * The renderer only reacts to a CHANGE of the pushed sequence index. Pushing the same index again is
 * dropped, and changing only Position or PlayRate while the index stays the same has no effect at
 * all, which is why restarting a one-shot requires the index itself to change. This processor
 * therefore leaves an already-correct index untouched instead of rewriting it every frame, and why
 * a gait's playback rate is applied at the moment the gait changes rather than tracking speed
 * continuously.
 *
 * The console variable crowdy.mass.anim.force overrides the whole selection with one fixed index on
 * every entity, for confirming that the pipeline from this fragment to the screen works at all.
 *
 * Also owns the one-shot action tier's Position correction: the exact frame an action's bank index
 * is first pushed is the only place that fact is known cheaply (it falls out of the push decision
 * already made here), so that is where the correction is applied, even though tracking an action's
 * elapsed time and expiring it belongs to UCrowdyMassActionProcessor instead. See
 * GetCrowdyMassActionCorrectedPosition for the reasoning.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassAnimationProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UCrowdyMassAnimationProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void InitializeInternal(UObject& Owner, const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	// The anim set resolved against the bank the entity actually renders through, resolved on first
	// use and reused until the bank changes under it. Resolution walks authored lists and touches
	// asset pointers, which is far too much to repeat per entity per frame at crowd sizes.
	const FCrowdyMassResolvedAnimSet* FindOrResolveAnimSet(const UCrowdyMassAnimSet* AnimSet, const FMassInstancedSkinnedMeshInfo& MeshInfo);

	FMassEntityQuery EntityQuery;

	// Written from inside the per-chunk loop, which is safe only because the query is walked one
	// chunk at a time. Anything that moves this processor onto a parallel chunk walk has to give this
	// map a lock or move it out of the loop first.
	TMap<FCrowdyMassAnimSetBankKey, FCrowdyMassResolvedAnimSet> ResolvedAnimSets;

	// Wall-clock time the next crowdy.mass.anim.dump summary may be logged, so holding the console
	// variable on does not print a block every frame.
	double NextDumpTimeSeconds = 0.0;

	// Wall-clock time the next crowdy.mass.anim.force summary may be logged. Forcing an index always
	// reports what it reached, whether or not the dump variable is on, because a silent run cannot be
	// told apart from a run that matched nothing.
	double NextForcedSummaryTimeSeconds = 0.0;

	// The forced index seen on the previous frame, so that first setting the variable can be answered
	// once rather than every frame.
	int32 PreviousForcedSequenceIndex = INDEX_NONE;

	// Entities that resolved no animation bank are reported whether or not any debug variable is on:
	// they produce silence, and silence here is otherwise the same reading as a crowd that is already
	// showing what it should.
	double NextUnresolvedWarningTimeSeconds = 0.0;

	double NextMissingProviderWarningTimeSeconds = 0.0;
};

/**
 * Development-time check that the batched animation data and the batched transforms handed to one
 * instanced skinned mesh component describe the same number of instances.
 *
 * Those two buffers are filled by separate processors and are joined by array position alone: entry
 * N of the animation buffer is applied to the instance named by entry N of the update buffer.
 * Nothing carries an entity identity across that join, so if the two ever hold a different number of
 * entries the instances silently start wearing each other's animations. Equal counts do not prove
 * the entries line up entity for entity, but a divergence is proof that they do not.
 *
 * An empty animation buffer against a non-empty update buffer is a different fault with a different
 * cause, so it is reported separately: it means no processor filled the animation batch at all, and
 * the instances are holding the last animation they were given rather than wearing each other's.
 *
 * Runs in the post-physics phase, which is after both buffers have been filled for the frame and
 * before the visualization component consumes and clears them.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassAnimationBatchAuditProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UCrowdyMassAnimationBatchAuditProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery EntityQuery;

	double NextDumpTimeSeconds = 0.0;

	// A divergence usually persists for many frames, so it is reported at an interval rather than
	// every frame. Kept per mesh component: with a single timer, a second component that starts
	// diverging while the first is still throttled would never be named at all.
	TMap<FObjectKey, double> NextMismatchLogTimeSecondsByComponent;
};
