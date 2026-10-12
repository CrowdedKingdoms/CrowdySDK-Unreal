#include "Processors/CrowdyRemoteTransformProcessor.h"

#include "Fragments/CrowdyMassFragments.h"
#include "Mass/EntityFragments.h"
#include "MassCommandBuffer.h"
#include "MassExecutionContext.h"
#include "MassMovementFragments.h"
#include "MassRepresentationTypes.h"

UCrowdyRemoteTransformProcessor::UCrowdyRemoteTransformProcessor()
	: EntityQuery(*this)
{
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::AllNetModes);
	ProcessingPhase = EMassProcessingPhase::PrePhysics;

	// This is what moves remote entities, so everything that looks at where an entity is has to run
	// after it: level of detail, which representation to draw it as, which animation suits how fast it
	// is going, and the batching that hands all of that to the renderer. Declared against the whole
	// representation group rather than any one of those passes, because otherwise the position they
	// read is a frame old or not, depending on the order modules happened to load in.
	ExecutionOrder.ExecuteBefore.Add(UE::Mass::ProcessorGroupNames::Representation);
}

void UCrowdyRemoteTransformProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.AddRequirement<FCrowdyRemoteInterpolationFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddRequirement<FMassVelocityFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddTagRequirement<FCrowdyRemoteInterpolatingTag>(EMassFragmentPresence::All);
}

void UCrowdyRemoteTransformProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	EntityQuery.ForEachEntityChunk(Context, [](FMassExecutionContext& Context)
	{
		const float DeltaTime = FMath::Max(0.0f, Context.GetDeltaTimeSeconds());
		TArrayView<FCrowdyRemoteInterpolationFragment> InterpolationFragments =	Context.GetMutableFragmentView<FCrowdyRemoteInterpolationFragment>();
		TArrayView<FTransformFragment> TransformFragments = Context.GetMutableFragmentView<FTransformFragment>();
		TArrayView<FMassVelocityFragment> VelocityFragments = Context.GetMutableFragmentView<FMassVelocityFragment>();

		for (FMassExecutionContext::FEntityIterator EntityIt = Context.CreateEntityIterator(); EntityIt; ++EntityIt)
		{
			FCrowdyRemoteInterpolationFragment& Interpolation = InterpolationFragments[EntityIt];
			FTransformFragment& TransformFragment = TransformFragments[EntityIt];
			FMassVelocityFragment& VelocityFragment = VelocityFragments[EntityIt];

			Interpolation.ElapsedSeconds += DeltaTime;
			const float Duration = FMath::Max(Interpolation.SmoothingDuration, UE_KINDA_SMALL_NUMBER);
			const float Alpha = FMath::Clamp(Interpolation.ElapsedSeconds / Duration, 0.0f, 1.0f);

			const FQuat StartRotation = Interpolation.StartRotation.GetNormalized();
			const FQuat TargetRotation = Interpolation.TargetRotation.GetNormalized();

			FTransform& Transform = TransformFragment.GetMutableTransform();
			const FVector PreviousLocation = Transform.GetLocation();
			const FVector NewLocation = FMath::Lerp(Interpolation.StartLocation, Interpolation.TargetLocation, Alpha);
			Transform.SetLocation(NewLocation);
			Transform.SetRotation(FQuat::Slerp(StartRotation, TargetRotation, Alpha).GetNormalized());
			// Scaled back up to the speed the sender was really travelling at. A segment is deliberately
			// replayed over a little longer than it took to arrive, so that the next update lands while
			// the entity is still moving instead of after it has stopped; that makes the entity cross
			// the ground more slowly than the player it stands for did. Animation picks a gait from
			// this value, so leaving it unscaled would make a whole crowd walk where it should jog.
			VelocityFragment.Value = DeltaTime > UE_KINDA_SMALL_NUMBER
				? ((NewLocation - PreviousLocation) / DeltaTime) * Interpolation.VelocityScale
				: FVector::ZeroVector;

			if (Alpha >= 1.0f)
			{
				Transform.SetLocation(Interpolation.TargetLocation);
				Transform.SetRotation(TargetRotation);
				VelocityFragment.Value = FVector::ZeroVector;
				Context.Defer().RemoveTag<FCrowdyRemoteInterpolatingTag>(Context.GetEntity(EntityIt));
			}
		}
	});
}
