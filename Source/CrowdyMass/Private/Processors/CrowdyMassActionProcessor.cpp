#include "Processors/CrowdyMassActionProcessor.h"

#include "CrowdyMassLog.h"
#include "Engine/World.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "MassExecutionContext.h"
#include "MassRepresentationTypes.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

namespace
{
	// How long an action that has never reached the renderer is kept before it is cleared anyway.
	// Sized to outlast one animation pass at a frame rate anyone would still call playable, so a real
	// action always gets its chance to be shown while one that can never resolve still goes away.
	constexpr float CrowdyMassUnshownActionGraceSeconds = 0.25f;
}

UCrowdyMassActionProcessor::UCrowdyMassActionProcessor()
	: EntityQuery(*this)
{
	// One-shot actions are cosmetic view state; a dedicated server has nothing to animate.
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Client | EProcessorExecutionFlags::Standalone);
	ProcessingPhase = EMassProcessingPhase::PrePhysics;

	ExecutionOrder.ExecuteInGroup = UE::Mass::ProcessorGroupNames::Representation;
	// Must land before the animation processor composes this frame's animation, so an action that
	// expires here is already gone rather than showing for one extra frame.
	ExecutionOrder.ExecuteBefore.Add(TEXT("CrowdyMassAnimationProcessor"));

	// Nothing here touches a UObject, an asset, or the representation subsystem: the pass reads and
	// writes floats and integers in one fragment array. That is what lets it run off the game thread,
	// which the animation processor beside it cannot do because resolving a bank is not thread safe.
	bRequiresGameThreadExecution = false;

	// Kept in the frame even on a config that carries no action fragment yet, so the ExecuteBefore
	// edge above is never silently dropped by pruning: a processor whose query currently matches no
	// archetype is cut from the frame entirely, taking every ordering edge naming it with it.
	QueryBasedPruning = EMassQueryBasedPruning::Never;
}

void UCrowdyMassActionProcessor::InitializeInternal(UObject& Owner, const TSharedRef<FMassEntityManager>& EntityManager)
{
	Super::InitializeInternal(Owner, EntityManager);

	// Never pruned, so it would run every frame on a map that draws no Crowdy rows; inactive, it still passes its ordering on.
	if (!UCrowdyMassEntitySubsystem::DrawsCrowdRows(Owner.GetWorld()))
	{
		MakeInactive();
	}
	UE_CLOG(!IsActive(), LogCrowdyMass, Verbose, TEXT("Crowdy Mass: %s sits out world '%s', which draws no crowd rows."), *GetName(), *GetNameSafe(Owner.GetWorld()));
}

void UCrowdyMassActionProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	// One fragment, read and written. Everything this pass needs about an action is on the entity by
	// the time it runs, so it claims no representation fragment, no mesh information, no shared anim
	// set and no subsystem: a narrower query is fewer arrays touched per chunk, and the arrays it does
	// not claim are never loaded at all.
	EntityQuery.AddRequirement<FCrowdyMassActionFragment>(EMassFragmentAccess::ReadWrite);
}

void UCrowdyMassActionProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	EntityQuery.ForEachEntityChunk(Context, [](FMassExecutionContext& Context)
	{
		TArrayView<FCrowdyMassActionFragment> ActionStates = Context.GetMutableFragmentView<FCrowdyMassActionFragment>();
		const float DeltaTimeSeconds = Context.GetDeltaTimeSeconds();

		for (FMassExecutionContext::FEntityIterator EntityIt = Context.CreateEntityIterator(); EntityIt; ++EntityIt)
		{
			FCrowdyMassActionFragment& ActionState = ActionStates[EntityIt];
			if (ActionState.ActionId == 0)
			{
				continue;
			}

			ActionState.ElapsedSecondsSinceTrigger += DeltaTimeSeconds;

			// An action the renderer has never been given cannot have finished playing, and its
			// duration is not known until it is shown, since that is where it is resolved and written.
			// This pass runs before the animation pass, so without the distinction a clip shorter than
			// one frame is cleared before it is ever composed and the player simply never sees it: a
			// 0.2 s hit flinch on a client at 5 fps, or any action at all on the long first frame after
			// a level load.
			//
			// The grace covers the other half, which is still wanted: an action that can never resolve
			// (an ActionId nothing authored, or a mesh with no bank) is never going to be shown, and
			// must not pin the entity on an active action for ever. It only has to outlast one
			// animation pass at a frame rate worth rendering at.
			const bool bHasBeenShown = ActionState.AppliedSerial == ActionState.Serial;
			const float ExpireAfterSeconds = bHasBeenShown
				? ActionState.DurationSeconds
				: CrowdyMassUnshownActionGraceSeconds;

			if (ActionState.ElapsedSecondsSinceTrigger >= ExpireAfterSeconds)
			{
				ActionState.ActionId = 0;
				ActionState.Variant = 0;
				ActionState.DurationSeconds = 0.0f;
				ActionState.ElapsedSecondsSinceTrigger = 0.0f;
				ActionState.AppliedSerial = ActionState.Serial;
			}
		}
	});
}

