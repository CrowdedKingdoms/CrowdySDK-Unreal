#include "Processors/CrowdyMassAnimationProcessor.h"

#include "Assets/CrowdyMassAnimSet.h"
#include "CrowdyMassLog.h"
#include "Engine/World.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "HAL/IConsoleManager.h"
#include "MassExecutionContext.h"
#include "MassMovementFragments.h"
#include "MassRepresentationFragments.h"
#include "MassRepresentationSubsystem.h"
#include "MassRepresentationTypes.h"
#include "MassSkinnedMeshRepresentationTypes.h"
#include "MassVisualizationComponent.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "UObject/Class.h"

namespace CrowdyMassAnimDebug
{
#if !UE_BUILD_SHIPPING
	int32 GForcedSequenceIndex = -1;
	FAutoConsoleVariableRef CVarForcedSequenceIndex(
		TEXT("crowdy.mass.anim.force"),
		GForcedSequenceIndex,
		TEXT("Force this animation bank sequence index on every Mass entity the Crowdy animation processor sees.\n")
		TEXT("A negative value disables forcing, which is the default."),
		ECVF_Cheat);

	int32 GTrace = 0;
	FAutoConsoleVariableRef CVarTrace(
		TEXT("crowdy.mass.anim.trace"),
		GTrace,
		TEXT("Log every animation sequence index the Crowdy animation processor writes, per entity."),
		ECVF_Cheat);

	int32 GDump = 0;
	FAutoConsoleVariableRef CVarDump(
		TEXT("crowdy.mass.anim.dump"),
		GDump,
		TEXT("While set to 1, log a summary of the animation sets in use and of the instance batches\n")
		TEXT("handed to each instanced skinned mesh component, once per second. Set back to 0 to stop."),
		ECVF_Cheat);
#else
	constexpr int32 GForcedSequenceIndex = -1;
	constexpr int32 GTrace = 0;
	constexpr int32 GDump = 0;
#endif

	// True at most once a second, so a message that would otherwise repeat every frame does not
	// scroll the console away. Each caller owns its own timestamp.
	bool HasThrottleElapsed(double& InOutNextTimeSeconds)
	{
		const double NowSeconds = FPlatformTime::Seconds();
		if (NowSeconds < InOutNextTimeSeconds)
		{
			return false;
		}

		InOutNextTimeSeconds = NowSeconds + 1.0;
		return true;
	}

	bool ShouldDumpNow(double& InOutNextDumpTimeSeconds)
	{
		return GDump != 0 && HasThrottleElapsed(InOutNextDumpTimeSeconds);
	}
}


UCrowdyMassAnimationProcessor::UCrowdyMassAnimationProcessor()
	: EntityQuery(*this)
{
	// Instanced skinned meshes only exist where there is something to draw, so a dedicated server has
	// no reason to run this.
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Client | EProcessorExecutionFlags::Standalone);
	ProcessingPhase = EMassProcessingPhase::PrePhysics;

	ExecutionOrder.ExecuteInGroup = UE::Mass::ProcessorGroupNames::Representation;
	ExecutionOrder.ExecuteAfter.Add(UE::Mass::ProcessorGroupNames::VisualizationProcessing);
	// Run after the processor that moves remote entities, because animation selection is driven by
	// how they moved.
	ExecutionOrder.ExecuteAfter.Add(TEXT("CrowdyRemoteTransformProcessor"));
	// The MetaHuman crowd's own animation processor writes the same fragment and clamps the sequence
	// index to the two poses it knows about. Running after it means a value written here survives;
	// running before it would have the value silently replaced. Projects that keep that processor
	// registered still get the index they asked for.
	ExecutionOrder.ExecuteAfter.Add(TEXT("MetaHumanMassSimpleAnimationProcessor"));
	// Run before every pass that forwards this fragment to the renderer, so the renderer sees this
	// frame's values rather than the previous frame's. Both the replacement below and the engine pass
	// it stands in for are named, because which of the two is registered is a project setting.
	ExecutionOrder.ExecuteBefore.Add(TEXT("CrowdyMassConsumeSkinnedMeshAnimationProcessor"));
	ExecutionOrder.ExecuteBefore.Add(TEXT("MassConsumeInstancedSkinnedMeshAnimationProcessor"));

	// Finding which animation bank an entity renders through means reading the representation
	// subsystem's shared mesh information, which is not thread safe. Requiring that subsystem below
	// pins this processor to the game thread on its own, because the subsystem declares no traits
	// saying it is safe to touch from a worker; stated here as well so the constraint is visible
	// where the cost is, rather than only as a side effect of a requirement further down.
	bRequiresGameThreadExecution = true;

	// Mass drops a processor from the frame entirely when no archetype matches its queries, which
	// would silently remove the debug output below in exactly the case it exists to explain: an
	// animation trait that is missing from the entity config. Staying in the frame costs one empty
	// pass, and only while the debug console variables are on, because Execute returns immediately
	// otherwise.
	QueryBasedPruning = EMassQueryBasedPruning::Never;
}

void UCrowdyMassAnimationProcessor::InitializeInternal(UObject& Owner, const TSharedRef<FMassEntityManager>& EntityManager)
{
	Super::InitializeInternal(Owner, EntityManager);

	// Never pruned, so it would run every frame on a map that draws no Crowdy rows; inactive, it still passes its ordering on.
	if (!UCrowdyMassEntitySubsystem::DrawsCrowdRows(Owner.GetWorld()))
	{
		MakeInactive();
	}
	UE_CLOG(!IsActive(), LogCrowdyMass, Verbose, TEXT("Crowdy Mass: %s sits out world '%s', which draws no crowd rows."), *GetName(), *GetNameSafe(Owner.GetWorld()));
}

void UCrowdyMassAnimationProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.AddRequirement<FCrowdyMassAnimStateFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddRequirement<FCrowdyMassActionFragment>(EMassFragmentAccess::ReadWrite);
	// Read only, and optional. Read only because its writers are the view-state ingestor and game
	// code, both outside the Mass schedule: this pass claiming a write would tell the scheduler
	// something untrue about who owns the byte. Optional because an archetype with no pose source
	// simply never holds one, and requiring it would narrow which entities animate at all.
	EntityQuery.AddRequirement<FCrowdyMassViewPoseFragment>(EMassFragmentAccess::ReadOnly, EMassFragmentPresence::Optional);
	EntityQuery.AddRequirement<FMassRepresentationAnimationFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddRequirement<FMassRepresentationFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassVelocityFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddConstSharedRequirement<FCrowdyMassAnimSetSharedFragment>();
	// Needed to look up the animation bank each entity renders through, which is what turns an
	// authored sequence into the index the renderer expects.
	//
	// The shared fragment is read only: all this pass takes from it is the subsystem pointer.
	// Claiming it for writing would serialize this pass against every other reader of that fragment
	// on every archetype carrying it, for nothing. The subsystem itself is claimed for writing
	// because the only accessor the engine exposes for the mesh information is a mutable one, and a
	// subsystem claim is compared without reference to which archetypes each pass touches: understate
	// it and the batching passes on either side could be scheduled alongside this one.
	EntityQuery.AddSharedRequirement<FMassRepresentationSubsystemSharedFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddSubsystemRequirement<UMassRepresentationSubsystem>(EMassFragmentAccess::ReadWrite);

	// Deliberately no chunk visibility filter and no static-representation exclusion. This processor
	// writes a fragment; it does not append to either of the instance buffers the renderer reads, so
	// narrowing it cannot help those buffers line up. What narrowing would do is leave a stale
	// animation on every entity that is off camera this frame, and make the counts reported below
	// describe part of the crowd rather than all of it. The pass that batches for the renderer is
	// where visibility filtering belongs.
}

void UCrowdyMassAnimationProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	const int32 ForcedSequenceIndex = CrowdyMassAnimDebug::GForcedSequenceIndex;
	const bool bForcing = ForcedSequenceIndex >= 0;
	const bool bTrace = CrowdyMassAnimDebug::GTrace != 0;
	const bool bDump = CrowdyMassAnimDebug::ShouldDumpNow(NextDumpTimeSeconds);

	// Index 0 is a real bank slot, but it is also the index an entity carries before anything has
	// written one, so forcing it looks identical to forcing nothing at all on an entity that has
	// never been given another index. Say so once, when it is first asked for.
	const bool bForcingFirstFrame = bForcing && PreviousForcedSequenceIndex != ForcedSequenceIndex;
	PreviousForcedSequenceIndex = ForcedSequenceIndex;

	UE_CLOG(bForcingFirstFrame && ForcedSequenceIndex == 0, LogCrowdyMass, Warning,
		TEXT("Crowdy Mass animation: sequence index 0 is also the index an entity starts on, so forcing it writes nothing on entities that have never been given another index and looks the same as a broken path. Force a different index to tell the two apart."));

	int32 MatchedEntities = 0;
	int32 SkinnedMeshInstances = 0;
	int32 SequenceIndexWrites = 0;
	int32 UnresolvedEntities = 0;
	TArray<const UCrowdyMassAnimSet*, TInlineAllocator<4>> AnimSets;

	EntityQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& Context)
	{
		UMassRepresentationSubsystem* RepresentationSubsystem = Context.GetSharedFragment<FMassRepresentationSubsystemSharedFragment>().RepresentationSubsystem;
		if (!RepresentationSubsystem)
		{
			return;
		}

		FMassInstancedSkinnedMeshInfoArrayView MeshInfos = RepresentationSubsystem->GetMutableInstancedSkinnedMeshInfos();

		const TConstArrayView<FMassRepresentationFragment> Representations = Context.GetFragmentView<FMassRepresentationFragment>();
		const TConstArrayView<FMassVelocityFragment> Velocities = Context.GetFragmentView<FMassVelocityFragment>();
		TArrayView<FMassRepresentationAnimationFragment> Animations = Context.GetMutableFragmentView<FMassRepresentationAnimationFragment>();
		TArrayView<FCrowdyMassAnimStateFragment> AnimStates = Context.GetMutableFragmentView<FCrowdyMassAnimStateFragment>();
		TArrayView<FCrowdyMassActionFragment> ActionStates = Context.GetMutableFragmentView<FCrowdyMassActionFragment>();
		const TConstArrayView<FCrowdyMassViewPoseFragment> ViewPoses = Context.GetFragmentView<FCrowdyMassViewPoseFragment>();

		const UCrowdyMassAnimSet* AnimSet = Context.GetConstSharedFragment<FCrowdyMassAnimSetSharedFragment>().AnimSet.Get();
		MatchedEntities += Context.GetNumEntities();

		if (bDump)
		{
			AnimSets.AddUnique(AnimSet);
		}

		// The smoothing weight depends only on this frame's length and the set's time constant, and
		// the blend length is a property of the set, so both are the same for every entity in the
		// chunk.
		const float DeltaTimeSeconds = Context.GetDeltaTimeSeconds();
		const float SmoothingAlpha = AnimSet
			? UCrowdyMassAnimSet::ComputeSmoothingAlpha(AnimSet->SpeedSmoothingTimeSeconds, DeltaTimeSeconds, AnimSet->MaxSmoothingStepSeconds)
			: 1.0f;
		const float BlendSeconds = AnimSet ? AnimSet->BlendSeconds : 0.0f;

		// Entities in one chunk almost always render through the same mesh description, so the
		// resolved set is looked up once and reused until an entity says otherwise.
		int32 ResolvedMeshInfoIndex = INDEX_NONE;
		const FCrowdyMassResolvedAnimSet* Resolved = nullptr;

		for (FMassExecutionContext::FEntityIterator EntityIt = Context.CreateEntityIterator(); EntityIt; ++EntityIt)
		{
			const FMassRepresentationFragment& Representation = Representations[EntityIt];
			SkinnedMeshInstances += Representation.CurrentRepresentation == EMassRepresentationType::SkinnedMeshInstance ? 1 : 0;

			FCrowdyMassAnimStateFragment& AnimState = AnimStates[EntityIt];
			FCrowdyMassActionFragment& ActionState = ActionStates[EntityIt];

			// Speed and gait are kept current for every entity, including one that is too far away to
			// be drawn as an instance this frame. Skipping those would leave an entity that comes back
			// into view showing whatever it was doing when it left, until it next changed gait.
			const float RawSpeed = static_cast<float>(Velocities[EntityIt].Value.Size2D());
			AnimState.SmoothedSpeed = UCrowdyMassAnimSet::AdvanceSmoothedSpeed(AnimState.SmoothedSpeed, RawSpeed, SmoothingAlpha);

			const int32 MeshInfoIndex = Representation.SkinnedMeshDescHandle.ToIndex();
			if (MeshInfoIndex != ResolvedMeshInfoIndex)
			{
				ResolvedMeshInfoIndex = MeshInfoIndex;
				Resolved = MeshInfos.IsValidIndex(MeshInfoIndex) ? FindOrResolveAnimSet(AnimSet, MeshInfos[MeshInfoIndex]) : nullptr;
			}

			// A forced index deliberately does not depend on any of this. It is the lever for answering
			// "does anything written here reach the screen at all", which is a question most worth
			// asking on a config that has no animation set assigned yet, so it must keep working when
			// nothing resolves.
			FCrowdyMassComposedAnimation Composed;
			if (Resolved)
			{
				AnimState.Gait = Resolved->SelectGait(AnimState.Gait, AnimState.SmoothedSpeed);
				const uint8 PoseId = ViewPoses.IsEmpty() ? 0 : ViewPoses[EntityIt].PoseId;
				Composed = ComposeCrowdyMassAnimation(AnimState, ActionState, PoseId, *Resolved, BlendSeconds);
			}
			else
			{
				++UnresolvedEntities;
			}

			const int32 DesiredSequenceIndex = bForcing ? ForcedSequenceIndex : Composed.BankIndex;

			FMassRepresentationAnimationFragment& Animation = Animations[EntityIt];
			const int32 PreviousSequenceIndex = Animation.AnimData.SequenceIndex;

			if (!ShouldPushCrowdyMassAnimation(DesiredSequenceIndex, PreviousSequenceIndex, AnimState.SequenceIndex))
			{
				continue;
			}

			AnimState.PlayRate = Composed.PlayRate;
			AnimState.LoopMode = Composed.LoopMode;
			AnimState.BlendSeconds = Composed.BlendSeconds;

			Animation.AnimData.SequenceIndex = DesiredSequenceIndex;
			Animation.AnimData.Position = 0.0f;
			Animation.AnimData.PlayRate = AnimState.PlayRate;
			Animation.AnimData.LoopMode = AnimState.LoopMode;
			// Written every time rather than left alone, because another writer may have set it and
			// the renderer would otherwise keep that value for this entity only.
			Animation.AnimData.BlendTime = AnimState.BlendSeconds;
			// Mirrored only once the write has happened, so this field never claims an index that was
			// not actually handed on.
			AnimState.SequenceIndex = DesiredSequenceIndex;
			++SequenceIndexWrites;

			// This is the exact frame the renderer's playback clock for this action starts counting
			// from, which is the only reason this correction lives at the push site rather than in
			// the processor that otherwise owns the action's lifetime: nothing else knows, this
			// cheaply, whether today's write is the first one for this Serial. Guarded on the trigger
			// not having been shown yet, so a later frame that re-affirms the same index can never
			// re-seek a clip that is already advancing on its own, and by !bForcing so the debug
			// override above (which pushes an unrelated index) can never be read as this action
			// having been shown.
			if (!bForcing && Composed.bIsAction && Composed.bIsFirstShowOfTrigger)
			{
				Animation.AnimData.Position = GetCrowdyMassActionCorrectedPosition(ActionState.ElapsedSecondsSinceTrigger, Composed.ActionDurationSeconds);

				// Recorded only once the push has actually happened, so this never claims a trigger the
				// renderer was not given. Skipped while forcing, because the debug override pushes an
				// unrelated index and marking the trigger shown would hide it once forcing stops.
				ActionState.AppliedSerial = ActionState.Serial;

				// Handed to the entity here because composing the action already resolved it. The pass
				// that expires the action then needs nothing but this float, instead of finding the
				// bank and searching the anim set by name for every active entity every frame.
				ActionState.DurationSeconds = Composed.ActionDurationSeconds;
			}

			UE_CLOG(bTrace, LogCrowdyMass, Log, TEXT("Crowdy Mass animation: entity %s sequence index %d -> %d, gait row %d, smoothed speed %.1f, play rate %.2f"),
				*LexToString(Context.GetEntity(EntityIt)), PreviousSequenceIndex, DesiredSequenceIndex,
				static_cast<int32>(AnimState.Gait), AnimState.SmoothedSpeed, AnimState.PlayRate);
		}
	});

	// A forced index reports what it reached every time, without the dump variable being on. Without
	// this line "no archetype carries the animation trait", "nothing is drawn as an instanced skinned
	// mesh yet" and "the index was written and something replaced it" all look identical: silence.
	const bool bSummarise = bDump || (bForcing && CrowdyMassAnimDebug::HasThrottleElapsed(NextForcedSummaryTimeSeconds));
	UE_CLOG(bSummarise, LogCrowdyMass, Log, TEXT("Crowdy Mass animation: %d entities matched, %d drawn as instanced skinned meshes, %d with no animation bank resolved, forced sequence index %d, %d sequence index writes this frame"),
		MatchedEntities, SkinnedMeshInstances, UnresolvedEntities, ForcedSequenceIndex, SequenceIndexWrites);

	// Zero writes is the expected steady state once every entity holds the index it wants, so it
	// cannot be read as a fault on its own. Entities that resolved nothing are a fault and would
	// otherwise be indistinguishable from that steady state in this same line, which is the shape
	// this whole summary exists to avoid.
	UE_CLOG(UnresolvedEntities > 0 && CrowdyMassAnimDebug::HasThrottleElapsed(NextUnresolvedWarningTimeSeconds), LogCrowdyMass, Warning,
		TEXT("Crowdy Mass animation: %d of %d entities have no animation bank to select from, so nothing is written for them. Check that the animation trait has an animation set assigned and that the mesh description carries an animation sequence transform provider."),
		UnresolvedEntities, MatchedEntities);

	if (!bDump)
	{
		return;
	}

	for (const UCrowdyMassAnimSet* AnimSet : AnimSets)
	{
		if (!AnimSet)
		{
			UE_LOG(LogCrowdyMass, Log, TEXT("  animation set: none assigned, so nothing can be resolved for these entities"));
			continue;
		}

		// The state enum is named because a pose set that resolves nothing and a pose set keyed on the
		// wrong enum look the same from the outside: entities that never leave their locomotion.
		const UEnum* StateEnum = AnimSet->GetPoseStateEnum();
		UE_LOG(LogCrowdyMass, Log, TEXT("  animation set '%s': %d gaits, %d actions, %d poses keyed on %s"),
			*AnimSet->GetName(), AnimSet->Gaits.Num(), AnimSet->Actions.Num(), AnimSet->Poses.Num(),
			StateEnum ? *StateEnum->GetName() : TEXT("no state enum"));
	}
}

const FCrowdyMassResolvedAnimSet* UCrowdyMassAnimationProcessor::FindOrResolveAnimSet(const UCrowdyMassAnimSet* AnimSet, const FMassInstancedSkinnedMeshInfo& MeshInfo)
{
	if (!AnimSet)
	{
		return nullptr;
	}

	// One mesh description can carry several meshes. Their banks are separate, so a sequence index
	// only means the same thing to all of them if they share one provider; resolving against the
	// first and reporting a second is the only honest reading of that.
	const UAnimSequenceTransformProviderData* Provider = nullptr;
	for (const FMassSkinnedMeshInstanceVisualizationMeshDesc& MeshDesc : MeshInfo.GetDesc().Meshes)
	{
		const UAnimSequenceTransformProviderData* MeshProvider = Cast<UAnimSequenceTransformProviderData>(MeshDesc.TransformProvider.Get());
		if (!MeshProvider)
		{
			continue;
		}

		if (!Provider)
		{
			Provider = MeshProvider;
			continue;
		}

		UE_CLOG(MeshProvider != Provider, LogCrowdyMass, Warning,
			TEXT("Crowdy Mass animation: mesh description carries more than one animation bank ('%s' and '%s'). A sequence index means a different animation in each, so animation is resolved against the first and the others will be wrong."),
			*GetNameSafe(Provider), *GetNameSafe(MeshProvider));
	}

	if (!Provider)
	{
		// Named rather than returned silently. Every entity on this description will animate nothing,
		// and the only other thing that would say so is the absence of output.
		UE_CLOG(CrowdyMassAnimDebug::HasThrottleElapsed(NextMissingProviderWarningTimeSeconds), LogCrowdyMass, Warning,
			TEXT("Crowdy Mass animation: the mesh description these entities render through carries no animation sequence transform provider, so there is no bank of sequences to select an index from and nothing can be animated."));
		return nullptr;
	}

	const FCrowdyMassAnimSetBankKey Key{FObjectKey(AnimSet), FObjectKey(Provider)};
	FCrowdyMassResolvedAnimSet& Entry = ResolvedAnimSets.FindOrAdd(Key);

	const TArray<FAnimSequenceTransformProviderSequence>& BankEntries = Provider->GetSequences();
	TArray<UAnimSequence*> BankSequences;
	BankSequences.Reserve(BankEntries.Num());
	for (const FAnimSequenceTransformProviderSequence& BankEntry : BankEntries)
	{
		BankSequences.Add(BankEntry.Sequence);
	}

	// Neither the bank nor the set normally changes while a level is running, but both can be re-authored
	// under a live editor session and each one goes stale in its own way. The bank's length is not enough
	// to notice a change to it: swapping one sequence for another, or reordering two, changes what every
	// index means while leaving the provider asset and the length alone, so the comparison is over the
	// contents. And a change to the set itself (a pose row added, a gait's speeds retyped, the state enum
	// repointed) leaves the bank identical while changing which rows this table has at all.
	if (Entry.MatchesSource(AnimSet, BankSequences))
	{
		return &Entry;
	}

	Entry.Build(AnimSet, BankSequences);
	// The one place a bank is first paired with an anim set on the game thread, which is where
	// authoring mistakes are worth reporting. The return value is deliberately unused: a set that
	// resolves only some of its entries still animates those, and refusing to use it would turn a
	// partly authored set into no animation at all.
	AnimSet->ValidateAgainstBank(BankSequences);

	// The mapped-value count is what separates "no state ever arrived" from "the state arrived and this
	// set maps none of its values to a row", which look identical on screen.
	UE_CLOG(CrowdyMassAnimDebug::GTrace != 0, LogCrowdyMass, Log, TEXT("Crowdy Mass animation: resolved animation set '%s' against bank '%s' (%d sequences), %d gait rows, %d state values mapped to a pose"),
		*AnimSet->GetName(), *Provider->GetName(), BankSequences.Num(),
		Entry.ResolvedGaits.Num(), Entry.PoseStateTable.CountMappedValues());

	return &Entry;
}

UCrowdyMassAnimationBatchAuditProcessor::UCrowdyMassAnimationBatchAuditProcessor()
	: EntityQuery(*this)
{
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::Client | EProcessorExecutionFlags::Standalone);
	// Both instance buffers are filled during the pre-physics phase and are consumed and cleared at
	// the end of the post-physics phase, so this is the window in which they can be compared.
	ProcessingPhase = EMassProcessingPhase::PostPhysics;
	bRequiresGameThreadExecution = true;

#if UE_BUILD_SHIPPING
	// A shipped game gains nothing from the check, so the processor stays out of the frame entirely.
	bAutoRegisterWithProcessingPhases = false;
#endif
}

void UCrowdyMassAnimationBatchAuditProcessor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.AddRequirement<FMassRepresentationFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddSharedRequirement<FMassRepresentationSubsystemSharedFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddSubsystemRequirement<UMassRepresentationSubsystem>(EMassFragmentAccess::ReadOnly);
}

void UCrowdyMassAnimationBatchAuditProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
#if !UE_BUILD_SHIPPING
	TArray<UMassRepresentationSubsystem*, TInlineAllocator<2>> RepresentationSubsystems;

	EntityQuery.ForEachEntityChunk(Context, [&RepresentationSubsystems](FMassExecutionContext& Context)
	{
		UMassRepresentationSubsystem* RepresentationSubsystem = Context.GetSharedFragment<FMassRepresentationSubsystemSharedFragment>().RepresentationSubsystem;
		if (RepresentationSubsystem)
		{
			RepresentationSubsystems.AddUnique(RepresentationSubsystem);
		}
	});

	if (RepresentationSubsystems.IsEmpty())
	{
		return;
	}

	const bool bDump = CrowdyMassAnimDebug::ShouldDumpNow(NextDumpTimeSeconds);

	for (const UMassRepresentationSubsystem* RepresentationSubsystem : RepresentationSubsystems)
	{
		const UMassVisualizationComponent* VisualizationComponent = RepresentationSubsystem->GetVisualizationComponent();
		if (!VisualizationComponent)
		{
			continue;
		}

		// The shared data entries are stored contiguously, so the first index that resolves to
		// nothing is the end of the list.
		for (int32 DescriptionIndex = 0; ; ++DescriptionIndex)
		{
			const FMassInstancedSkinnedMeshComponentSharedData* SharedData = VisualizationComponent->GetSharedDataForDescriptionIndex(DescriptionIndex);
			if (!SharedData)
			{
				break;
			}

			const UInstancedSkinnedMeshComponent* MeshComponent = SharedData->GetInstancedSkinnedMeshComponent();
			if (!MeshComponent)
			{
				continue;
			}

			const int32 InstanceUpdateCount = SharedData->GetEntitiesRequiringUpdate().Num();
			const int32 TransformCount = SharedData->GetMeshInstanceTransforms().Num();
			const int32 AnimationCount = SharedData->GetMeshInstanceAnimationData().Num();

			// The batching appends an instance's handle and its transform in one step, and drops them
			// in one step, so these two counts describe the same instances and cannot legitimately
			// differ. Only the transform count is reported below; this guards the assumption that
			// lets one number stand for both.
			ensureMsgf(InstanceUpdateCount == TransformCount,
				TEXT("Crowdy Mass: '%s' batched %d instance handles against %d instance transforms."),
				*MeshComponent->GetName(), InstanceUpdateCount, TransformCount);

			UE_CLOG(bDump, LogCrowdyMass, Log, TEXT("Crowdy Mass animation batches on '%s': %d instances, %d animation entries, %d animation tracks, %d live instances"),
				*MeshComponent->GetName(), TransformCount, AnimationCount,
				SharedData->GetEntityToTrackMap().Num(), SharedData->GetEntityToPrimitiveIdMap().Num());

			if (AnimationCount == TransformCount)
			{
				continue;
			}

			if (!CrowdyMassAnimDebug::HasThrottleElapsed(NextMismatchLogTimeSecondsByComponent.FindOrAdd(FObjectKey(MeshComponent))))
			{
				continue;
			}

			if (AnimationCount == 0)
			{
				UE_LOG(LogCrowdyMass, Warning, TEXT("Crowdy Mass: '%s' batched %d instances this frame and no animation data at all, so no processor is filling its animation batch. Its instances keep whatever animation they were last given, and nothing written to the animation fragment reaches the renderer."),
					*MeshComponent->GetName(), TransformCount);
				continue;
			}

			if (AnimationCount > TransformCount)
			{
				UE_LOG(LogCrowdyMass, Warning, TEXT("Crowdy Mass: '%s' batched %d animation entries against %d instances, so more than one processor is filling this component's animation batch. The SDK switches MassConsumeInstancedSkinnedMeshAnimationProcessor off at startup, so something turned it back on (Project Settings, Mass, or a DefaultMass.ini setting bAutoRegisterWithProcessingPhases=True); remove that so only one pass fills it."),
					*MeshComponent->GetName(), AnimationCount, TransformCount);
				continue;
			}

			UE_LOG(LogCrowdyMass, Warning, TEXT("Crowdy Mass: '%s' batched %d animation entries against %d instances. Animation entries are matched to instances by array position, so from the first position the two disagree on, instances are drawn with another instance's animation."),
				*MeshComponent->GetName(), AnimationCount, TransformCount);
		}
	}

	if (!bDump)
	{
		return;
	}

	const UWorld* World = Context.GetWorld();
	const UCrowdyMassEntitySubsystem* EntitySubsystem = World ? World->GetSubsystem<UCrowdyMassEntitySubsystem>() : nullptr;
	if (EntitySubsystem)
	{
		UE_LOG(LogCrowdyMass, Log, TEXT("Crowdy Mass animation batches: %d tracked remote entities"), EntitySubsystem->GetTrackedRemoteEntityCount());
	}
#endif
}
