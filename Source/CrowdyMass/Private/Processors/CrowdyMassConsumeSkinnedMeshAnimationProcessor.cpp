#include "Processors/CrowdyMassConsumeSkinnedMeshAnimationProcessor.h"

#include "Animation/AnimSequenceTransformProviderData.h"
#include "CrowdyMassLog.h"
#include "Mass/EntityFragments.h"
#include "MassExecutionContext.h"
#include "MassLODFragments.h"
#include "MassRepresentationAnimationProcessor.h"
#include "MassRepresentationFragments.h"
#include "MassRepresentationSubsystem.h"
#include "MassRepresentationTypes.h"
#include "MassSkinnedMeshRepresentationTypes.h"

#if !UE_BUILD_SHIPPING
namespace CrowdyMassConsumeAnimDiagnostics
{
	struct FAnimationBank
	{
		const UAnimSequenceTransformProviderData* Provider = nullptr;
		int32 NumSequences = 0;
	};

	// One mesh description can carry several meshes, each with its own animation bank. A pushed
	// sequence index has to be valid for all of them, so the smallest bank decides the valid range.
	FAnimationBank FindSmallestAnimationBank(const FMassInstancedSkinnedMeshInfo& MeshInfo)
	{
		FAnimationBank Smallest;

		for (const FMassSkinnedMeshInstanceVisualizationMeshDesc& MeshDesc : MeshInfo.GetDesc().Meshes)
		{
			const UAnimSequenceTransformProviderData* Provider = Cast<UAnimSequenceTransformProviderData>(MeshDesc.TransformProvider.Get());
			if (!Provider)
			{
				continue;
			}

			const int32 NumSequences = Provider->GetSequences().Num();
			if (Smallest.Provider && Smallest.NumSequences <= NumSequences)
			{
				continue;
			}

			Smallest.Provider = Provider;
			Smallest.NumSequences = NumSequences;
		}

		return Smallest;
	}

	// One distinct sequence index pushed against one animation bank, and how many instances pushed it.
	struct FPushedIndexSample
	{
		int32 MeshInfoIndex = INDEX_NONE;
		int32 SequenceIndex = INDEX_NONE;
		int32 BankSize = 0;
		int32 EntityCount = 0;
		bool bHasBank = false;
		FString BankName;
	};

	// Enough distinct samples to describe any sane frame, and a hard stop so a runaway index cannot
	// grow the list without bound.
	constexpr int32 MaxPushedIndexSamples = 32;
}
#endif

UCrowdyMassConsumeSkinnedMeshAnimationProcessor::UCrowdyMassConsumeSkinnedMeshAnimationProcessor()
	: EntityQuery(*this)
{
	// Instanced skinned meshes only exist where there is something to draw, so a dedicated server has
	// no reason to run this.
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Client | EProcessorExecutionFlags::Standalone);

	ExecutionOrder.ExecuteInGroup = UE::Mass::ProcessorGroupNames::Representation;
	ExecutionOrder.ExecuteAfter.Add(UE::Mass::ProcessorGroupNames::VisualizationProcessing);
	// The transform batching overwrites the previous LOD significance it shares with this pass, and
	// both passes need the value from before that overwrite to decide whether an entity crossed a
	// significance boundary this frame. Running first is what keeps the two batches the same length
	// when an entity does cross one.
	ExecutionOrder.ExecuteBefore.Add(TEXT("MassUpdateInstancedSkinnedMeshProcessor"));
	// This pass stands in for the engine one, but a project that has not yet deregistered the engine
	// pass runs both, and both append to the same animation batch. Appending first keeps this pass's
	// entries at the positions the renderer actually reads, so the entries the engine pass adds land
	// past the end of the instance list and are ignored rather than shifting every entity's
	// animation onto its neighbour. Once the engine pass is deregistered this name resolves to
	// nothing and the ordering is simply unused.
	ExecutionOrder.ExecuteBefore.Add(TEXT("MassConsumeInstancedSkinnedMeshAnimationProcessor"));

	// Touches the representation subsystem's shared mesh info, which is not thread safe.
	bRequiresGameThreadExecution = true;
}

void UCrowdyMassConsumeSkinnedMeshAnimationProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	// Every clause that affects which archetypes and chunks are matched mirrors the processor that
	// batches instance transforms, because the two batches are joined by array position and have to
	// come out the same length. The transform fragment is required and never read for that reason
	// alone: a requirement is a match condition, and leaving it out would let this processor match an
	// archetype the transform processor skips.
	EntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassRepresentationFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassRepresentationAnimationFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassRepresentationLODFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddChunkRequirement<FMassVisualizationChunkFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.SetChunkFilter(&FMassVisualizationChunkFragment::AreAnyEntitiesVisibleInChunk);
	EntityQuery.AddSharedRequirement<FMassRepresentationSubsystemSharedFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddSubsystemRequirement<UMassRepresentationSubsystem>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddTagRequirement<FMassStaticRepresentationTag>(EMassFragmentPresence::None);
}

void UCrowdyMassConsumeSkinnedMeshAnimationProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	EntityQuery.ForEachEntityChunk(Context, [](FMassExecutionContext& Context)
	{
		UMassRepresentationSubsystem* RepresentationSubsystem = Context.GetSharedFragment<FMassRepresentationSubsystemSharedFragment>().RepresentationSubsystem;
		check(RepresentationSubsystem);

		FMassInstancedSkinnedMeshInfoArrayView MeshInfos = RepresentationSubsystem->GetMutableInstancedSkinnedMeshInfos();

		const TConstArrayView<FMassRepresentationFragment> Representations = Context.GetFragmentView<FMassRepresentationFragment>();
		const TConstArrayView<FMassRepresentationLODFragment> RepresentationLODs = Context.GetFragmentView<FMassRepresentationLODFragment>();
		const TConstArrayView<FMassRepresentationAnimationFragment> Animations = Context.GetFragmentView<FMassRepresentationAnimationFragment>();

		for (FMassExecutionContext::FEntityIterator EntityIt = Context.CreateEntityIterator(); EntityIt; ++EntityIt)
		{
			const FMassRepresentationFragment& Representation = Representations[EntityIt];
			if (Representation.CurrentRepresentation != EMassRepresentationType::SkinnedMeshInstance)
			{
				continue;
			}

			const int32 MeshInfoIndex = Representation.SkinnedMeshDescHandle.ToIndex();
			if (!MeshInfos.IsValidIndex(MeshInfoIndex))
			{
				continue;
			}

			UMassConsumeInstancedSkinnedMeshAnimationProcessor::UpdateMeshAnimation(
				Context.GetEntity(EntityIt), MeshInfos[MeshInfoIndex], Animations[EntityIt].AnimData,
				RepresentationLODs[EntityIt].LODSignificance, Representation.PrevLODSignificance);
		}
	});

#if !UE_BUILD_SHIPPING
	ReportSequenceIndicesOutsideTheBank(Context);
#endif
}

#if !UE_BUILD_SHIPPING
void UCrowdyMassConsumeSkinnedMeshAnimationProcessor::ReportSequenceIndicesOutsideTheBank(FMassExecutionContext& Context)
{
	using namespace CrowdyMassConsumeAnimDiagnostics;

	const double NowSeconds = FPlatformTime::Seconds();
	if (NowSeconds < NextValidationTimeSeconds)
	{
		return;
	}

	NextValidationTimeSeconds = NowSeconds + 1.0;

	TArray<FPushedIndexSample, TInlineAllocator<8>> Samples;

	EntityQuery.ForEachEntityChunk(Context, [&Samples](FMassExecutionContext& Context)
	{
		UMassRepresentationSubsystem* RepresentationSubsystem = Context.GetSharedFragment<FMassRepresentationSubsystemSharedFragment>().RepresentationSubsystem;
		if (!RepresentationSubsystem)
		{
			return;
		}

		FMassInstancedSkinnedMeshInfoArrayView MeshInfos = RepresentationSubsystem->GetMutableInstancedSkinnedMeshInfos();

		const TConstArrayView<FMassRepresentationFragment> Representations = Context.GetFragmentView<FMassRepresentationFragment>();
		const TConstArrayView<FMassRepresentationAnimationFragment> Animations = Context.GetFragmentView<FMassRepresentationAnimationFragment>();

		for (FMassExecutionContext::FEntityIterator EntityIt = Context.CreateEntityIterator(); EntityIt; ++EntityIt)
		{
			const FMassRepresentationFragment& Representation = Representations[EntityIt];
			if (Representation.CurrentRepresentation != EMassRepresentationType::SkinnedMeshInstance)
			{
				continue;
			}

			const int32 MeshInfoIndex = Representation.SkinnedMeshDescHandle.ToIndex();
			if (!MeshInfos.IsValidIndex(MeshInfoIndex))
			{
				continue;
			}

			const int32 SequenceIndex = Animations[EntityIt].AnimData.SequenceIndex;
			FPushedIndexSample* Sample = Samples.FindByPredicate([MeshInfoIndex, SequenceIndex](const FPushedIndexSample& Candidate)
			{
				return Candidate.MeshInfoIndex == MeshInfoIndex && Candidate.SequenceIndex == SequenceIndex;
			});

			if (!Sample)
			{
				if (Samples.Num() >= MaxPushedIndexSamples)
				{
					continue;
				}

				const FAnimationBank Bank = FindSmallestAnimationBank(MeshInfos[MeshInfoIndex]);
				Sample = &Samples.AddDefaulted_GetRef();
				Sample->MeshInfoIndex = MeshInfoIndex;
				Sample->SequenceIndex = SequenceIndex;
				Sample->BankSize = Bank.NumSequences;
				Sample->bHasBank = Bank.Provider != nullptr;
				Sample->BankName = GetNameSafe(Bank.Provider);
			}

			++Sample->EntityCount;
		}
	});

	for (const FPushedIndexSample& Sample : Samples)
	{
		// A description with no animation bank at all has no range to be outside of, and every entity
		// on it starts at index 0, so reporting those would be noise rather than a finding.
		if (!Sample.bHasBank)
		{
			continue;
		}

		// A bank that exists but holds nothing is a different problem: every index is rejected, so
		// none of these instances can animate at all.
		if (Sample.BankSize <= 0)
		{
			UE_LOG(LogCrowdyMass, Warning, TEXT("Crowdy Mass animation: animation bank '%s' holds no sequences, so every index pushed to it is rejected and its %d instance(s) cannot animate."),
				*Sample.BankName, Sample.EntityCount);
			continue;
		}

		if (Sample.SequenceIndex >= 0 && Sample.SequenceIndex < Sample.BankSize)
		{
			continue;
		}

		UE_LOG(LogCrowdyMass, Warning, TEXT("Crowdy Mass animation: sequence index %d was pushed for %d instance(s) of animation bank '%s', which holds %d sequences, so the only valid indices are 0 to %d. The renderer drops an index outside that range without complaint and leaves those instances on the animation they already had."),
			Sample.SequenceIndex, Sample.EntityCount, *Sample.BankName, Sample.BankSize, Sample.BankSize - 1);
	}
}
#endif
