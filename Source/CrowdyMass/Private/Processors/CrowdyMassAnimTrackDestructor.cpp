#include "Processors/CrowdyMassAnimTrackDestructor.h"

#include "CrowdyMassLog.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "MassExecutionContext.h"
#include "MassRepresentationFragments.h"
#include "MassRepresentationSubsystem.h"
#include "MassRepresentationTypes.h"
#include "MassSkinnedMeshRepresentationTypes.h"
#include "Subsystems/CrowdyMassVisualReleaseQueue.h"

namespace CrowdyMassAnimTrackDebug
{
#if !UE_BUILD_SHIPPING
	// Deliberately reintroduces a defect, which is why it is a cheat variable and why it exists at all:
	// the acceptance gate that measures animation tracks across a spawn and despawn cycle has to be
	// shown failing, or a run that passes says only that the gate was run, not that the thing it
	// watches for is being prevented. Set to 1, churn, and the track count should fail to return to
	// its baseline. Set back to 0 and it should return.
	int32 GReleaseFromObserver = 0;
	FAutoConsoleVariableRef CVarReleaseFromObserver(
		TEXT("crowdy.mass.anim.releasefromobserver"),
		GReleaseFromObserver,
		TEXT("Release an entity's renderer visual straight from the destruction observer instead of deferring it to the next frame.\n")
		TEXT("This is the behaviour the deferral replaced and it leaks an animation track per entity destroyed mid-frame.\n")
		TEXT("It exists so the animation track gate can be proven to fail. 0 is correct behaviour and the default."),
		ECVF_Cheat);
#else
	constexpr int32 GReleaseFromObserver = 0;
#endif

	// True at most once a second, so a line that would otherwise print for every batch of destroyed
	// entities does not scroll the console away. Each caller owns its own timestamp.
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

	// Deliberately shares the animation processor's dump variable instead of adding a second one: the
	// counts below only mean anything read against the live track and instance counts that variable
	// already prints. It is registered in another file, so this is a lookup by name and it runs every
	// time it is called. Callers therefore put the once-a-second throttle first, which costs one lookup a
	// second rather than one per pass, at the price of consuming that second's slot whether or not
	// anything is printed. In a shipped build the variable does not exist at all and the lookup answers
	// no.
	bool IsDumpEnabled()
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.mass.anim.dump"));
		return Variable && Variable->GetInt() != 0;
	}

	// Whether the entity's fragment could name an instanced skinned mesh visual at all. Necessary and not
	// sufficient: it is the cheap read that keeps entities with no skinned visual out of the lookups
	// below, and it does not say the entity ever held one.
	//
	// Neither value is written by anything that batches. The description handle is filled in when the
	// entity is built, from its visualization trait, and the previous LOD significance is written the
	// first frame the entity leaves having no representation at all, whichever representation it is
	// heading for. An entity that spawns already close enough to be given its own actor therefore passes
	// both without ever having been drawn as an instance.
	//
	// The previous significance is also required non-negative because it is what names the range holding
	// the instance, and a negative value names no range at all.
	bool CouldHoldSkinnedMeshVisual(const FMassRepresentationFragment& Representation)
	{
		return Representation.SkinnedMeshDescHandle.IsValid() && Representation.PrevLODSignificance >= 0.0f;
	}

	// Whether the entity is being drawn as an instanced row right now.
	//
	// This covers the one window the renderer's record below cannot. A batch is applied later than it is
	// made: nothing is recorded against an entity until the renderer walks the batch, and an entity
	// batched and then destroyed before that walk still has its instance added and its track allocated by
	// it. Reading the current representation catches that entity, because the pass that decides
	// representation runs ahead of the passes that batch, so it cannot have changed in between.
	bool IsDrawnAsSkinnedMeshInstance(const FMassRepresentationFragment& Representation)
	{
		return Representation.CurrentRepresentation == EMassRepresentationType::SkinnedMeshInstance;
	}

	// Whether any mesh component in this significance range still holds an animation track for the entity.
	// This is the renderer's own record and the only thing that says the entity really was drawn as an
	// instance, so it is what separates a release that frees a track from one that asks the renderer to
	// remove an instance that never existed.
	//
	// It asks what the entity was BATCHED as, not what it is being drawn as now, and the difference is the
	// whole of the entity that gets its own actor. Being given an actor does not release anything: the
	// sweep that drops the entity's instance during the batch update erases the instance id and never
	// touches the track, which only the removal path deallocates and un-records. So an entity destroyed
	// while drawn by an actor still answers true here, which is exactly the case that has to be released,
	// while its current representation and, a frame later, its previous one both name that actor.
	bool RangeHoldsAnimationTrack(FMassLODInstancedSkinnedMeshSignificanceRange& Range, const FMassEntityHandle Entity)
	{
		if (!Range.InstancedSkinnedMeshSharedDataPtr)
		{
			return false;
		}

		for (const FInstancedSkinnedMeshComponentSharedDataKey& ComponentRef : Range.SkinnedMeshComponentRefs)
		{
			const FMassInstancedSkinnedMeshComponentSharedData* SharedData = Range.InstancedSkinnedMeshSharedDataPtr->Find(ComponentRef);
			if (SharedData && SharedData->GetEntityToTrackMap().FindId(Entity).IsValid())
			{
				return true;
			}
		}

		return false;
	}
}

UCrowdyMassAnimTrackDestructor::UCrowdyMassAnimTrackDestructor()
	: EntityQuery(*this)
{
	// Destroying an entity removes every fragment it carries, so the removal of the representation
	// fragment is where an entity that was being drawn says it is going away.
	ObservedTypes.Add(FMassRepresentationFragment::StaticStruct());
	ObservedOperations = EMassObservedOperationFlags::Remove;

	// Runs in every world mode rather than only the ones expected to draw a crowd. Where nothing is drawn
	// there is no mesh information to walk and this returns immediately, so the cost of running somewhere
	// unnecessary is nothing, while the cost of not running somewhere necessary is an animation track
	// leaked per entity for the lifetime of the process.
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::AllWorldModes);

	// Reads the representation subsystem's mesh descriptions and writes the world subsystem's release
	// queue, neither of which has any synchronisation of its own.
	bRequiresGameThreadExecution = true;
}

void UCrowdyMassAnimTrackDestructor::ConfigureQueries(const TSharedRef<FMassEntityManager>& EntityManager)
{
	EntityQuery.AddRequirement<FMassRepresentationFragment>(EMassFragmentAccess::ReadWrite);
	EntityQuery.AddSharedRequirement<FMassRepresentationSubsystemSharedFragment>(EMassFragmentAccess::ReadWrite);

	// Stationary entities already hand their instances back through the engine's own destructor, which
	// requires this tag. Excluding the tag here, rather than simply not asking for it, is what keeps the
	// two passes disjoint: if the tag were merely absent from this query, an entity carrying it would be
	// handed back by both.
	EntityQuery.AddTagRequirement<FMassStaticRepresentationTag>(EMassFragmentPresence::None);

	// The engine's destructor also requires the representation parameters, which it never reads. That
	// requirement is dropped here: an entity carrying a representation fragment without them would
	// otherwise be skipped in silence, and leak exactly the track this pass exists to release.
}

void UCrowdyMassAnimTrackDestructor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	const UWorld* World = Context.GetWorld();
	UCrowdyMassVisualReleaseQueue* ReleaseQueue = World ? World->GetSubsystem<UCrowdyMassVisualReleaseQueue>() : nullptr;

	// Only editor worlds lack the queue, and nothing draws a skinned crowd there that needs a deferred release.
	if (!ReleaseQueue)
	{
		return;
	}

	int32 Observed = 0;
	int32 NoSubsystem = 0;
	int32 NoDescriptionTable = 0;
	int32 NeverBatched = 0;
	int32 UnknownDescriptions = 0;
	int32 DescriptionsWithoutComponents = 0;
	int32 NoRange = 0;
	int32 Queued = 0;
	int32 RefusedByFullQueue = 0;

	// Only ever non-zero while the cheat variable above is on, which is a deliberately broken mode. It
	// is counted rather than left silent so a run that measured nothing cannot be mistaken for a run
	// that measured correct behaviour.
	int32 ReleasedImmediately = 0;
	float UnmatchedSignificance = 0.0f;

	EntityQuery.ForEachEntityChunk(Context, [&](FMassExecutionContext& Context)
	{
		// Counted before either guard below. A chunk that hits one of them was still matched by this
		// pass, and counting it only afterwards would make a crowd with no mesh descriptions registered
		// read as a pass that matched nothing at all, which points a reader at the query rather than at
		// the registration.
		const int32 NumEntities = Context.GetNumEntities();
		Observed += NumEntities;

		UMassRepresentationSubsystem* RepresentationSubsystem = Context.GetMutableSharedFragment<FMassRepresentationSubsystemSharedFragment>().RepresentationSubsystem;
		if (!RepresentationSubsystem)
		{
			NoSubsystem += NumEntities;
			return;
		}

		// Resolved once for the whole chunk. The accessor reaches through the subsystem to the
		// visualization component and, in a build with the access detector on, opens a scoped write
		// claim, so it is not something to repeat per entity.
		FMassInstancedSkinnedMeshInfoArrayView MeshInfos = RepresentationSubsystem->GetMutableInstancedSkinnedMeshInfos();
		if (MeshInfos.Num() == 0)
		{
			NoDescriptionTable += NumEntities;
			return;
		}

		const TArrayView<FMassRepresentationFragment> Representations = Context.GetMutableFragmentView<FMassRepresentationFragment>();

		// Filled per entity and handed over once at the end of the chunk, so the loop below stays a walk
		// over fragments with no calls out of it.
		ChunkReleases.Reset();

		for (FMassExecutionContext::FEntityIterator EntityIt = Context.CreateEntityIterator(); EntityIt; ++EntityIt)
		{
			FMassRepresentationFragment& Representation = Representations[EntityIt];
			const FMassEntityHandle Entity = Context.GetEntity(EntityIt);

			// Checked before the lookups below, because neither a missing description nor an
			// unmatched range can be a leak for an entity that could not be holding a track at all.
			if (!CrowdyMassAnimTrackDebug::CouldHoldSkinnedMeshVisual(Representation))
			{
				++NeverBatched;
				continue;
			}

			// The previous significance is what names the range currently holding the instance: the
			// current one is where the entity was heading, and is only reached once a batch update has
			// moved it there.
			const float LODSignificance = Representation.PrevLODSignificance;
			const int32 MeshInfoIndex = Representation.SkinnedMeshDescHandle.ToIndex();

			// Read before the write below clears it, because it is one of the two things that say this
			// entity holds a visual worth handing back.
			const bool bDrawnAsRow = CrowdyMassAnimTrackDebug::IsDrawnAsSkinnedMeshInstance(Representation);

			// The fragment stops naming a live visual here, before any of the checks below can fail,
			// because the entity is going away either way. Anything that reads it during the rest of
			// teardown then sees that rather than a handle to something already released.
			Representation.CurrentRepresentation = EMassRepresentationType::None;
			Representation.SetIsWaitingForActorVisualReadiness(false);

			// The handle is read off a fragment that is in the middle of being removed, so it is treated
			// as a value that may name nothing. An out of range index is reported and skipped rather than
			// trusted, because indexing with it would crash the teardown.
			if (!MeshInfos.IsValidIndex(MeshInfoIndex))
			{
				++UnknownDescriptions;
				continue;
			}

			FMassInstancedSkinnedMeshInfo& MeshInfo = MeshInfos[MeshInfoIndex];
			if (!MeshInfo.IsValid())
			{
				// This description holds no mesh components, so there is nothing to hand an instance back
				// to. Either it was torn down, which destroyed every track recorded against it, or its
				// components have not been built yet and no track was ever allocated. Neither leaves
				// anything behind.
				++DescriptionsWithoutComponents;
				continue;
			}

			// Resolved here only to tell a record that can be released apart from one that cannot, and to
			// read the track record below. The range is not kept beyond this entity: the description
			// table can be rebuilt before the record is drained, which would leave a stored pointer
			// naming freed memory, so the drain re-resolves it from the index and the significance
			// recorded below.
			FMassLODInstancedSkinnedMeshSignificanceRange* Range = MeshInfo.GetLODSignificanceRange(LODSignificance);
			if (!Range)
			{
				++NoRange;
				UnmatchedSignificance = LODSignificance;
				continue;
			}

			// An entity being drawn as a row, or one the renderer holds a track for, is holding a visual
			// that has to be handed back. An entity that is neither is holding nothing: queueing it would
			// spend a slot in a capped queue to ask the renderer to remove an instance it never had, and
			// would count it as released, which is what empties out the number below that says whether
			// the leak is closed.
			if (!bDrawnAsRow && !CrowdyMassAnimTrackDebug::RangeHoldsAnimationTrack(*Range, Entity))
			{
				++NeverBatched;
				continue;
			}

			// Restores the behaviour this deferral replaced, so the gate that measures the animation
			// track count can be shown to fail when the release happens at the wrong moment. Releasing
			// straight from here looks correct and is not: the entity may already be sitting in this
			// frame's pending-update batch, and the pass that drains removals runs before the pass that
			// walks that batch, so the instance is released and then immediately re-added with a fresh
			// animation track keyed to an entity that no longer exists. Nothing can release that one.
			//
			// A measurement that only ever passes proves nothing, and this is the one lever that makes
			// the difference visible from a console rather than from a rebuild.
			if (CrowdyMassAnimTrackDebug::GReleaseFromObserver != 0)
			{
				MeshInfo.RemoveInstance(Entity, LODSignificance);
				++ReleasedImmediately;
				continue;
			}

			FCrowdyMassPendingVisualRelease& Release = ChunkReleases.AddDefaulted_GetRef();
			Release.RepresentationSubsystem = RepresentationSubsystem;
			Release.Entity = Entity;
			Release.MeshDescriptionIndex = MeshInfoIndex;
			Release.LODSignificance = LODSignificance;
		}

		if (ChunkReleases.IsEmpty())
		{
			return;
		}

		const int32 RecordedCount = ReleaseQueue->QueueVisualReleases(ChunkReleases);
		Queued += RecordedCount;
		RefusedByFullQueue += ChunkReleases.Num() - RecordedCount;
	});

	ObservedEntities += Observed;
	EntitiesWithNoRepresentationSubsystem += NoSubsystem;
	EntitiesWithNoMeshDescriptionTable += NoDescriptionTable;
	EntitiesNeverBatched += NeverBatched;
	EntitiesWithUnknownMeshDescription += UnknownDescriptions;
	EntitiesOnMeshDescriptionWithoutComponents += DescriptionsWithoutComponents;
	EntitiesWithNoSignificanceRange += NoRange;
	ReleasesQueued += Queued;
	ReleasesRefusedByFullQueue += RefusedByFullQueue;
	ReleasesIssuedFromObserver += ReleasedImmediately;

	// Said loudly and every time, not throttled and not behind the dump variable. A session running in
	// this mode is measuring a defect on purpose, and the one way for that to go wrong is for somebody
	// to leave it on and later read a leaking track count as a real regression.
	UE_CLOG(ReleasedImmediately > 0, LogCrowdyMass, Warning,
		TEXT("Crowdy Mass animation tracks: crowdy.mass.anim.releasefromobserver is on, so %d entity visuals were released straight from the observer instead of being deferred. This is the behaviour that leaks an animation track per entity destroyed mid-frame and exists only to prove the track gate can fail. Set it back to 0."),
		ReleasedImmediately);

	if (NoRange > 0)
	{
		LastUnmatchedSignificance = UnmatchedSignificance;
	}

	// Says what this pass did and nothing more. A queued release is a request, not an outcome: whether an
	// animation track was actually freed is the track count against the live instance count printed
	// beside this line under the same console variable.
	UE_CLOG(CrowdyMassAnimTrackDebug::HasThrottleElapsed(NextSummaryTimeSeconds) && CrowdyMassAnimTrackDebug::IsDumpEnabled(), LogCrowdyMass, Log,
		TEXT("Crowdy Mass animation tracks: %d destroyed entities seen, %d queued for release, %d holding no instanced skinned mesh visual, %d on a mesh description with no components, %d with no representation subsystem, %d with no mesh descriptions registered, %d refused by a full queue, %d tracks left allocated"),
		ObservedEntities, ReleasesQueued, EntitiesNeverBatched, EntitiesOnMeshDescriptionWithoutComponents,
		EntitiesWithNoRepresentationSubsystem, EntitiesWithNoMeshDescriptionTable,
		ReleasesRefusedByFullQueue, EntitiesWithUnknownMeshDescription + EntitiesWithNoSignificanceRange);

	UE_CLOG(UnknownDescriptions > 0 && CrowdyMassAnimTrackDebug::HasThrottleElapsed(NextUnknownDescriptionWarningTimeSeconds), LogCrowdyMass, Warning,
		TEXT("Crowdy Mass animation tracks: %d entities were destroyed naming a mesh description that no longer exists, so nothing could be asked whether they held an animation track and any they do hold stays allocated for the rest of the session."),
		UnknownDescriptions);

	// The value is named because it is the whole diagnosis. Whether these entities hold a track cannot be
	// told either way: the record that answers that is kept per significance range, and this is the count
	// of entities whose significance resolved to none.
	UE_CLOG(NoRange > 0 && CrowdyMassAnimTrackDebug::HasThrottleElapsed(NextNoRangeWarningTimeSeconds), LogCrowdyMass, Warning,
		TEXT("Crowdy Mass animation tracks: %d entities were destroyed whose previous LOD significance (%.3f) matches no significance range on their mesh description, so nothing could be told to release them. Any animation track they hold stays allocated for the rest of the session."),
		NoRange, LastUnmatchedSignificance);
}

UCrowdyMassVisualReleaseProcessor::UCrowdyMassVisualReleaseProcessor()
{
	ProcessingPhase = EMassProcessingPhase::PrePhysics;

	// Ahead of every pass that batches instances for the renderer. A release queued after the frame's
	// batching would be drained by the renderer in the same pass that adds the entity back, which
	// re-allocates the animation track this exists to free.
	ExecutionOrder.ExecuteBefore.Add(UE::Mass::ProcessorGroupNames::Representation);

	// Matches the pass that queues the releases, for the same reason: a queue that fills in a world where
	// nothing drains it leaks a track per record and then starts refusing records.
	ExecutionFlags = static_cast<int32>(EProcessorExecutionFlags::AllWorldModes);

	// Handing an instance back appends to the per-component buffers held by the representation subsystem.
	// Those are plain arrays with no synchronisation of their own, and the visualization component reads
	// and clears them on the game thread. Every pass that batches into the same buffers is pinned to the
	// game thread too, so pinning this one is also what stops it overlapping them.
	bRequiresGameThreadExecution = true;

	// This pass drains a world subsystem's queue and owns no entity query, so Mass would otherwise cut it
	// from the frame for matching no archetype. That would remove the only code that releases a track and
	// leave the queue growing in silence.
	QueryBasedPruning = EMassQueryBasedPruning::Never;
}

void UCrowdyMassVisualReleaseProcessor::Execute(FMassEntityManager& EntityManager, FMassExecutionContext& Context)
{
	const UWorld* World = Context.GetWorld();
	UCrowdyMassVisualReleaseQueue* ReleaseQueue = World ? World->GetSubsystem<UCrowdyMassVisualReleaseQueue>() : nullptr;
	if (!ReleaseQueue || ReleaseQueue->GetPendingVisualReleaseCount() == 0)
	{
		return;
	}

	ReleaseQueue->DrainPendingVisualReleases(Releases);
	if (Releases.IsEmpty())
	{
		return;
	}

	int32 Issued = 0;
	int32 NoRange = 0;
	int32 DepartedSubsystems = 0;
	int32 DepartedDescriptions = 0;
	float UnmatchedSignificance = 0.0f;

	// Records are grouped by representation subsystem rather than resolved one at a time, because the
	// accessor that reaches the mesh descriptions opens a scoped write claim in a build with the access
	// detector on and only one claim may be open at a time. A world runs one such subsystem in almost
	// every case, so this is one group and one claim for the whole drain.
	TArray<TWeakObjectPtr<UMassRepresentationSubsystem>, TInlineAllocator<2>> Subsystems;
	for (const FCrowdyMassPendingVisualRelease& Release : Releases)
	{
		Subsystems.AddUnique(Release.RepresentationSubsystem);
	}

	for (const TWeakObjectPtr<UMassRepresentationSubsystem>& WeakSubsystem : Subsystems)
	{
		UMassRepresentationSubsystem* RepresentationSubsystem = WeakSubsystem.Get();
		if (!RepresentationSubsystem)
		{
			for (const FCrowdyMassPendingVisualRelease& Release : Releases)
			{
				// The subsystem holding these visuals has gone, and with it every mesh component and
				// every animation track recorded against one. Nothing is left allocated, so this is
				// counted apart from a release that could not be issued against a live component.
				DepartedSubsystems += Release.RepresentationSubsystem == WeakSubsystem ? 1 : 0;
			}
			continue;
		}

		FMassInstancedSkinnedMeshInfoArrayView MeshInfos = RepresentationSubsystem->GetMutableInstancedSkinnedMeshInfos();

		for (const FCrowdyMassPendingVisualRelease& Release : Releases)
		{
			if (Release.RepresentationSubsystem != WeakSubsystem)
			{
				continue;
			}

			// Re-checked here rather than trusted from the frame the record was made. A mesh description
			// can be registered or removed in between, which both moves what an index means and frees the
			// ranges a stored pointer would have named.
			if (!MeshInfos.IsValidIndex(Release.MeshDescriptionIndex))
			{
				++DepartedDescriptions;
				continue;
			}

			FMassInstancedSkinnedMeshInfo& MeshInfo = MeshInfos[Release.MeshDescriptionIndex];
			if (!MeshInfo.IsValid())
			{
				++DepartedDescriptions;
				continue;
			}

			FMassLODInstancedSkinnedMeshSignificanceRange* Range = MeshInfo.GetLODSignificanceRange(Release.LODSignificance);
			if (!Range)
			{
				++NoRange;
				UnmatchedSignificance = Release.LODSignificance;
				continue;
			}

			// Puts the entity on the mesh component's removal queue. The instance is removed and its
			// animation track deallocated when that component next processes the queue, which is at the
			// start of the post-physics phase, after this frame's batching has run and found nothing to
			// batch for an entity that no longer exists.
			Range->RemoveInstance(Release.Entity);
			++Issued;
		}
	}

	ReleasesDrained += Releases.Num();
	ReleasesIssued += Issued;
	ReleasesOnDepartedSubsystem += DepartedSubsystems;
	ReleasesOnDepartedDescription += DepartedDescriptions;
	ReleasesWithNoSignificanceRange += NoRange;

	if (NoRange > 0)
	{
		LastUnmatchedSignificance = UnmatchedSignificance;
	}

	Releases.Reset();

	// Said once, without any console variable being on, because this pass never running and this pass
	// having nothing to do both look like an empty log. It reports that the release was ISSUED, which is
	// as far as this pass can see: the renderer frees the track later in the frame, and the reading that
	// answers whether it did is the track count against the live instance count.
	if (Issued > 0 && !bReportedFirstRelease)
	{
		bReportedFirstRelease = true;
		UE_LOG(LogCrowdyMass, Log, TEXT("Crowdy Mass animation tracks: %d entity visuals handed to the renderer for release, the first time this has happened. That is the request being made, not a track being freed. Turn on crowdy.mass.anim.dump and read the animation track count against the live instance count: those two following each other is what says the tracks are being released, and a track count that only ever rises is what says they are not."),
			Issued);
	}

	UE_CLOG(CrowdyMassAnimTrackDebug::HasThrottleElapsed(NextSummaryTimeSeconds) && CrowdyMassAnimTrackDebug::IsDumpEnabled(), LogCrowdyMass, Log,
		TEXT("Crowdy Mass animation tracks: %d releases drained, %d issued to the renderer, %d whose mesh description had already gone, %d whose representation subsystem had already gone, %d matching no significance range"),
		ReleasesDrained, ReleasesIssued, ReleasesOnDepartedDescription, ReleasesOnDepartedSubsystem,
		ReleasesWithNoSignificanceRange);

	UE_CLOG(NoRange > 0 && CrowdyMassAnimTrackDebug::HasThrottleElapsed(NextNoRangeWarningTimeSeconds), LogCrowdyMass, Warning,
		TEXT("Crowdy Mass animation tracks: %d queued releases could not be issued because their LOD significance (%.3f) matches no significance range on a mesh description that is still live, so the significance ranges changed between the entity being destroyed and its visual being handed back. Any animation track those entities hold stays allocated for the rest of the session."),
		NoRange, LastUnmatchedSignificance);
}
