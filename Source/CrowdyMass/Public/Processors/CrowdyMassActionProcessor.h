#pragma once

#include "CoreMinimal.h"
#include "MassEntityQuery.h"
#include "MassProcessor.h"
#include "CrowdyMassActionProcessor.generated.h"

struct FMassInstancedSkinnedMeshInfo;
class UCrowdyMassAnimSet;

/**
 * Owns the lifetime of a one-shot action once it has been triggered on a Mass entity's
 * FCrowdyMassActionFragment: keeps ElapsedSecondsSinceTrigger current every frame the action is
 * active, and clears ActionId once the action's authored duration has passed, returning the
 * entity to whatever the animation compose rule shows underneath it (a held pose, or its gait).
 * Active is the fragment's ActionId != 0, never a tag: a tag add or
 * remove is an archetype move, and this fires on every trigger of every one-shot in the crowd.
 *
 * Deliberately does not decide whether an action's animation has actually reached the renderer
 * yet, nor write the first-applied-frame Position correction: only the animation processor's push
 * decision knows, on the one frame it happens, whether this frame's write is a genuinely new index
 * or a repeat, and duplicating that decision here would be two sources of truth for one fact. This
 * processor only tracks how long an action has been alive and when it should end.
 *
 * Runs before the animation processor in the same group, so an action that expires this frame is
 * already inactive by the time this frame's animation is composed, instead of lingering one extra
 * frame on a clip that has already finished.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassActionProcessor : public UMassProcessor
{
	GENERATED_BODY()

public:
	UCrowdyMassActionProcessor();

protected:
	virtual void ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void InitializeInternal(UObject& Owner, const TSharedRef<FMassEntityManager>& EntityManager) override;
	virtual void Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context) override;

private:
	FMassEntityQuery EntityQuery;
};
