#pragma once

#include "CoreMinimal.h"
#include "MassEntityQuery.h"
#include "MassProcessor.h"
#include "CrowdyMassConsumeSkinnedMeshAnimationProcessor.generated.h"

/**
 * Hands each entity's animation data to the instanced skinned mesh batching.
 *
 * This stands in for the engine's own animation consume processor, which has to be kept out of the
 * frame or every entity is batched twice; the CrowdyMass module deregisters it at startup.
 *
 * The reason for standing in at all is that the engine version matches a wider set of entities than
 * the processor that batches instance transforms: it has no chunk visibility filter and does not
 * exclude entities whose representation is static. The two buffers are joined by array position
 * alone, with nothing carrying an entity identity across the join, so the moment their lengths
 * diverge every instance from that position on is drawn with another instance's animation. The query
 * below therefore mirrors the transform processor clause for clause, which is what keeps the two
 * buffers the same length.
 *
 * The batching itself is not reimplemented. The engine exports its batching entry point and this
 * processor calls it, so later changes to how a batch is built are inherited rather than duplicated.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassConsumeSkinnedMeshAnimationProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UCrowdyMassConsumeSkinnedMeshAnimationProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
#if !UE_BUILD_SHIPPING
	// An animation bank sequence index outside the bank's range is rejected by the renderer without
	// any complaint, so nothing further down the path can report it. This walks the same entities a
	// second time and names the bad index, which is why it is throttled rather than run every frame.
	void ReportSequenceIndicesOutsideTheBank(FMassExecutionContext& Context);
#endif

	FMassEntityQuery EntityQuery;

#if !UE_BUILD_SHIPPING
	// Wall-clock time the next out-of-range sweep may run. Bank contents do not change from frame to
	// frame, so sampling once a second surfaces a bad index without doubling the per-frame walk.
	double NextValidationTimeSeconds = 0.0;
#endif
};
