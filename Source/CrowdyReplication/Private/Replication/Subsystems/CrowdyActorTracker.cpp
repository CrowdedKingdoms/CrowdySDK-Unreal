// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "CrowdyReplicationLog.h"

#include "HAL/Event.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Replication/Subsystems/CrowdyActorUpdateRouting.h"
#include "Replication/Subsystems/CrowdyShardScheduling.h"
#include "Subsystem/CrowdyWorkerThreadsSubsystem.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace
{
	// Several frames of a full receive budget, so an ordinary hitch keeps everything and only a stall
	// that the world tick is not answering at all reaches it.
	TAutoConsoleVariable<int32> CVarCrowdyTrackerMaxGatheredUpdates(
		TEXT("crowdy.replication.tracker.maxgatheredupdates"),
		8192,
		TEXT("How many actor updates for already-tracked actors may be gathered before the backlog is "
			"discarded (default 8192). Reached only when the world tick is not consuming them."),
		ECVF_Default);
}

const FInstancedStruct& FCrowdyActorUpdate::ResolveState() const
{
	// The retained message where the internal chain put it, the copied property where a Blueprint delegate
	// left one. Never both, and an update that has neither reads as an unset struct rather than a null.
	return Message.IsValid() ? Message->State : State;
}

void UCrowdyActorTracker::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	const UWorld* World = GetWorld();

	if (!IsValid(World))
		return;

	// During UGameEngine::Init the initial Game world is created before a
	// UGameInstance owns it, so GetGameInstance() can be null here. Bail out
	// rather than dereferencing it.
	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
	{
		UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Verbose, TEXT("[Crowdy Actor Tracker]: No GameInstance yet; skipping init."));
		return;
	}

	WorkerThreadsSubsystem = GameInstance->GetSubsystem<UCrowdyWorkerThreadsSubsystem>();
	UCrowdyGameSession* GameSession = GameInstance->GetSubsystem<UCrowdyGameSession>();
	UCrowdySDKBridgeSubsystem* CrowdyBridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();

	check(IsValid(WorkerThreadsSubsystem.Get()));
	check(IsValid(GameSession));
	check(IsValid(CrowdyBridge));
	
	if (!IsValid(WorkerThreadsSubsystem.Get()))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Tracker]: Invalid Worker Threads subsystem."));
		return;
	}
	
	GameSession->OnOwnerUUIDUpdated.AddDynamic(this, &UCrowdyActorTracker::SetLocalUUID);
	
	if (!LoadDeveloperSettings())
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[Crowdy Actor Tracker]: Disabled or error in config of Developer Settings."));
		return;
	}
		
	SetupQueues();
	
	if (CrowdyBridge->ServiceRegistry)
	{
		ActorUpdateSubscription = CrowdyBridge->ServiceRegistry->SubscribeToAllPayloads(
			ECrowdyPayloadCategory::ActorUpdate,
			{ ECrowdySubscriptionRole::Observe, /*bRequiresExclusiveHandling*/ false, TEXT("CrowdyActorTracker") },
			[this](const FCrowdyDelivery& Delivery) { HandleActorUpdateDelivery(Delivery); });
	}

	World->GetTimerManager().SetTimer(TimeoutTimerHandle, 
		this, 
		&UCrowdyActorTracker::CheckTimeouts, 
		ActorTimeoutThreshold, 
		true);
	
	UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[Crowdy Actor Tracker]: Initialized."));
}

void UCrowdyActorTracker::Deinitialize()
{
	// Releasing the handle stops any further delivery to this subscriber, including later in the same fan-out, so
	// this needs no lookup back through the bridge. See FCrowdyDelivery for the threading rules.
	ActorUpdateSubscription.Release();

	if (const UWorld* World = GetWorld())
		World->GetTimerManager().ClearTimer(TimeoutTimerHandle);

	// Stop handing shards out, then wait for whatever is already reading one. Both matter: the consumers read
	// state that is about to go away, and a queue may only be drained while nothing else is reading it.
	bShuttingDown.store(true, std::memory_order_release);

	// Dropped rather than flushed: what these would be handed to goes away with the world.
	TrackedUpdates.Empty();

	for (FEvent* Event : ShardEvents)
	{
		if (Event)
			Event->Trigger();
	}

	// Only done once it is certain that no consumer is left. Running out of patience does not mean they are gone:
	// this subsystem goes away with its world while the worker pool lives on the game instance, so a consumer
	// that is merely late is the usual reason for it. Draining a shard queue while one is still reading it would
	// make this a second reader of a queue that allows exactly one, and taking an item off frees the node the
	// other reader is looking at.
	if (WaitForShardTasks())
	{
		for (const TUniquePtr<TQueue<FCrowdyActorUpdate, EQueueMode::Mpsc>>& Queue : UpdateQueues)
		{
			FCrowdyActorUpdate Discarded;
			while (Queue->Dequeue(Discarded)) {}
		}

		for (FEvent* Event : ShardEvents)
		{
			FPlatformProcess::ReturnSynchEventToPool(Event);
		}

		ShardEvents.Empty();
	}
	// Otherwise both are left alone. The queues are destroyed with this object regardless, so draining them buys
	// nothing, and an event is only ever given back once it is certain nothing is waiting on it: handing one back
	// while a waiter is still on it would let it be reissued elsewhere and signalled underneath that waiter. Never
	// returning it costs one event per shard, on a path that only happens as a world is going away.

	Super::Deinitialize();
}

bool UCrowdyActorTracker::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World)
	{
		return false;
	}

	// PIE or Game only
	if (World->WorldType != EWorldType::PIE &&
		World->WorldType != EWorldType::Game)
	{
		return false;
	}
	
	return true;
}

void UCrowdyActorTracker::HandleActorUpdateDelivery(const FCrowdyDelivery& Delivery)
{
	// The busiest subscriber handler in the SDK: it runs once per received actor update and holds
	// close to half of what delivering one costs. The scopes nested below it split that half, because
	// an outer scope alone only says the cost is somewhere inside, and there are three separate
	// per-update costs in here: this state copy, the queue node, and the wake of the consumer.
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_TrackerHandleActorUpdate);

	// AllPayloads(ActorUpdate) only ever carries ACTOR_UPDATE_NOTIFICATION (130), so the opcode is
	// implied by the subscription; nothing else routes here.
	const auto& AUN = Delivery.GetAs<FActorUpdateNotificationMessage>();

	FCrowdyActorUpdate Update;
	Update.UUID = AUN.GUID;

	{
		// Retained rather than copied. FCrowdyDelivery allows keeping the message past the call and its
		// decoded State is owned, so this is a refcount increment where it used to be an allocation and a
		// deep copy. What the retain does NOT keep alive is the frame's octets, so nothing downstream may
		// read the message's views; ResolveState is the only door and it reaches only the owned State.
		TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_TrackerRetainState);
		Update.Message = StaticCastSharedRef<const FActorUpdateNotificationMessage>(Delivery.Message);
	}

	Update.ServerTimestamp = AUN.Timestamp;
	Update.ClassID = static_cast<int64>(AUN.PayloadClassID);
	EnqueueUpdate(MoveTemp(Update));
}

void UCrowdyActorTracker::Configure(const int32 InMaxTrackedActors, const int32 InMaxUpdatesPerBatch, const float InMaxBatchWaitTime,
                                    const float InActorTimeoutThreshold)
{
	// Clamped here because this is the only way these are set, from Blueprint and from the map profile alike, and
	// the ranges declared on the config struct are a details panel hint that ini text and Blueprint both bypass.
	// None of these have a meaningful value at or below zero: a tracked actor capacity of zero divides by zero on
	// the first update, and a batch limit of zero gathers nothing while looking full, so a consumer would hand its
	// shard to a successor that gathers nothing either and no update would ever be delivered again.
	MaxTrackedActors      = FMath::Max(1, InMaxTrackedActors);
	MaxUpdatesPerBatch    = FMath::Max(1, InMaxUpdatesPerBatch);
	MaxBatchWaitTime      = FMath::Max(0.0f, InMaxBatchWaitTime);
	ActorTimeoutThreshold = FMath::Max(0.0f, InActorTimeoutThreshold);

	// Rebuild sets with new capacity
	TrackedUUIDs  = MakeUnique<FCrowdyGuidSet>(MaxTrackedActors);
	PendingSpawns = MakeUnique<FCrowdyGuidSet>(MaxTrackedActors);
}

void UCrowdyActorTracker::ToggleOwnerTracking(const bool bEnable)
{
	bOwnerTrackingEnabled = bEnable;
}

void UCrowdyActorTracker::ToggleBroadcastUpdatesToGameThread(const bool bEnable)
{
	bBroadcastUpdatesToGameThread = bEnable;
}

void UCrowdyActorTracker::SetupQueues()
{
	NumOfShards = CrowdyShardScheduling::ComputeShardCount(FPlatformMisc::NumberOfCores());

	UpdateQueues.SetNum(NumOfShards);
	ShardStates.SetNum(NumOfShards);
	ShardEvents.SetNumZeroed(NumOfShards);

	for (int32 i = 0; i < NumOfShards; i++)
	{
		UpdateQueues[i] = MakeUnique<TQueue<FCrowdyActorUpdate, EQueueMode::Mpsc>>();
		ShardStates[i].store(CrowdyShardScheduling::EShardState::Idle, std::memory_order_relaxed);

		ShardEvents[i] = FPlatformProcess::GetSynchEventFromPool(false);
	}
	
	TrackedUUIDs = MakeUnique<FCrowdyGuidSet>(MaxTrackedActors);
	PendingSpawns = MakeUnique<FCrowdyGuidSet>(MaxTrackedActors);

	LastUpdateTimes.Reserve(MaxTrackedActors);
}

void UCrowdyActorTracker::EnqueueUpdate(const FCrowdyActorUpdate& Update)
{
	FCrowdyActorUpdate Copy = Update;
	EnqueueUpdate(MoveTemp(Copy));
}

void UCrowdyActorTracker::EnqueueUpdate(FCrowdyActorUpdate&& Update)
{
	if (Update.UUID == LocalUUID && !bOwnerTrackingEnabled)
		return;

	// Nothing was set up, so there is nowhere to put this. Reachable when the map profile turned the tracker off.
	if (UpdateQueues.IsEmpty())
		return;

	const bool bTracked = TrackedUUIDs.IsValid() && TrackedUUIDs->Contains(Update.UUID);

	if (CrowdyActorUpdateRouting::RouteOnReceive(bTracked) == CrowdyActorUpdateRouting::EReceiveRoute::Gather)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_TrackerGatherTracked);

		// Receiving is driven by the core ticker and gathering is drained by the world tick, so a paused or
		// stalled world keeps taking updates in while nothing takes them out. Bounded here because a remote
		// sender decides the rate: without it a long pause grows this until the client runs out of memory.
		if (TrackedUpdates.Num() >= GetMaxGatheredUpdates())
		{
			DiscardGatheredBacklog();
		}

		TrackedUpdates.Add(MoveTemp(Update));
		return;
	}

	int32 ShardIndex = 0;
	{
		// Hashing a GUID and taking a modulo is a handful of instructions, so what this scope reads is
		// very nearly the cost of a scope itself. That makes it the noise floor the other scopes in
		// this function have to be read against, rather than a cost anyone would act on.
		TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_TrackerShardHash);
		ShardIndex = GetTypeHash(Update.UUID) % NumOfShards;
	}

	{
		// The queue allocates a node per item, so this is the second heap allocation an update pays on
		// its way in, after the state copy.
		TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_TrackerQueueEnqueue);
		UpdateQueues[ShardIndex]->Enqueue(MoveTemp(Update));
	}

	{
		// A compare exchange that under load finds the shard already taken and returns without
		// scheduling anything.
		TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_TrackerClaim);
		CrowdyShardScheduling::ClaimShardForWork(
			ShardStates[ShardIndex],
			[this, ShardIndex]() { return ScheduleShardTask(ShardIndex); },
			[this, ShardIndex]()
			{
				// One signal per batch window rather than one per update, because the claim only
				// reaches here on the first update after a consumer was last told to look again.
				// Scoped so the count stays visible: it is the number of scheduler wakes the receive
				// path pays for, and it used to be one per received update.
				TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_TrackerWake);

				if (FEvent* Event = ShardEvents.IsValidIndex(ShardIndex) ? ShardEvents[ShardIndex] : nullptr)
					Event->Trigger();
			});
	}
}

void UCrowdyActorTracker::Tick(float DeltaTime)
{
	FlushTrackedUpdates();
}

int32 UCrowdyActorTracker::GetMaxGatheredUpdates()
{
	return FMath::Max(1, CVarCrowdyTrackerMaxGatheredUpdates.GetValueOnGameThread());
}

void UCrowdyActorTracker::DiscardGatheredBacklog()
{
	// The newest positions are the ones worth keeping, so the backlog goes rather than the update that
	// found the ceiling. These are superseded views of where an actor is, on a transport that already
	// drops, and the next update for each actor corrects it.
	const int32 Discarded = TrackedUpdates.Num();

	// Recorded anyway, or an actor whose updates are being discarded looks silent and is timed out and
	// despawned, which is a far worse answer than a stale position.
	{
		const double Now = FPlatformTime::Seconds();
		FWriteScopeLock W(LastUpdateLock);
		for (const FCrowdyActorUpdate& Update : TrackedUpdates)
			LastUpdateTimes.Add(Update.UUID, Now);
	}

	TrackedUpdates.Reset();

	if (bReportedGatheredBacklogDiscarded.exchange(true, std::memory_order_acq_rel))
		return;

	UE_LOG(LogCrowdyReplication, Warning,
		TEXT("[Crowdy Actor Tracker]: Discarded %d gathered actor updates on reaching the %d limit, because they "
			"are arriving faster than the world tick is consuming them. Reported once."),
		Discarded, GetMaxGatheredUpdates());
}

void UCrowdyActorTracker::FlushTrackedUpdates()
{
	if (TrackedUpdates.IsEmpty())
		return;

	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_TrackerFlushTracked);

	{
		const double Now = FPlatformTime::Seconds();
		FWriteScopeLock W(LastUpdateLock);
		for (const FCrowdyActorUpdate& Update : TrackedUpdates)
			LastUpdateTimes.Add(Update.UUID, Now);
	}

	OnTrackedActorUpdates.Broadcast(TrackedUpdates);

	if (bBroadcastUpdatesToGameThread)
	{
		// The retained message cannot cross the dynamic delegate below, which marshals property by
		// property, so the state is materialised into the reflected property. Built as a separate batch
		// rather than in place: what was just broadcast above is only safe to alter if every subscriber
		// took a copy, and that is a subscriber's choice rather than something stated here.
		TArray<FCrowdyActorUpdate> ReflectedBatch = TrackedUpdates;

		for (FCrowdyActorUpdate& Update : ReflectedBatch)
		{
			Update.State = Update.ResolveState();
			Update.Message.Reset();
		}

		OnUpdatesGameThread.Broadcast(ReflectedBatch);
	}

	TrackedUpdates.Reset();
}

bool UCrowdyActorTracker::ScheduleShardTask(const int32 ShardIndex)
{
	// Allocates a task array and goes through the worker pool, so it is far dearer than the enqueue
	// that leads here. It should be rare while updates keep arriving, because it only runs for a shard
	// that was found idle, and the count is what confirms it rather than assuming it.
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_TrackerScheduleShard);

	if (bShuttingDown.load(std::memory_order_acquire))
		return false;

	UCrowdyWorkerThreadsSubsystem* Pool = WorkerThreadsSubsystem.Get();
	if (!IsValid(Pool))
	{
		ReportSchedulingUnavailable();
		return false;
	}

	TArray<TFunction<void()>> Tasks;
	Tasks.Add([this, ShardIndex]()
	{
		CrowdyShardScheduling::RunShardConsumer(
			ShardStates[ShardIndex],
			[this, ShardIndex]() { ProcessQueue(ShardIndex); },
			[this, ShardIndex]() { return !UpdateQueues[ShardIndex]->IsEmpty(); },
			[this, ShardIndex]() { return ScheduleShardTask(ShardIndex); },
			[this]() { ShardTasksInFlight.fetch_sub(1, std::memory_order_acq_rel); });
	});

	const bool bScheduled = CrowdyShardScheduling::ScheduleShardConsumer(
		ShardTasksInFlight,
		[Pool, &Tasks]() { return Pool->EnqueueTasks(Tasks); });

	if (!bScheduled)
		ReportSchedulingUnavailable();

	return bScheduled;
}

bool UCrowdyActorTracker::WaitForShardTasks()
{
	// A consumer gives up as soon as shutdown is flagged, so in practice this returns immediately. It is bounded
	// anyway: a task handed to a worker pool that has since stopped will never run, and waiting for one that
	// cannot arrive would hang the level teardown instead.
	constexpr double MaxWaitSeconds = 0.25;
	const double Deadline = FPlatformTime::Seconds() + MaxWaitSeconds;

	while (ShardTasksInFlight.load(std::memory_order_acquire) > 0)
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("[Crowdy Actor Tracker]: %d actor update task(s) were still outstanding %.0f ms into shutdown. They are either still running or were queued behind a worker pool that has already stopped."),
				ShardTasksInFlight.load(std::memory_order_acquire), MaxWaitSeconds * 1000.0);
			return false;
		}

		// A whole millisecond, so this actually blocks. Anything that rounds to zero milliseconds becomes a bare
		// yield, which returns immediately when no thread of equal priority is ready to run, and the workers this
		// is waiting on run at a higher priority than the game thread. That would burn a core flat out for the
		// whole wait, on a machine already busy enough to have made the wait necessary.
		FPlatformProcess::Sleep(0.001f);
	}

	return true;
}

void UCrowdyActorTracker::WaitForShardWork(const int32 ShardIndex, const double RemainingSeconds)
{
	// This runs on the consumer, not on the receive path, and its time is mostly time spent blocked
	// rather than time spent working, so the duration is not a cost. The count is the point: a
	// consumer can only be woken by a signal if it got as far as this line, so this count against the
	// number of signals sent is what says how many of those signals had a waiter to wake.
	TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_TrackerConsumerWait);

	FEvent* Event = ShardEvents.IsValidIndex(ShardIndex) ? ShardEvents[ShardIndex] : nullptr;
	if (!Event)
	{
		FPlatformProcess::Sleep(0.001f);
		return;
	}

	Event->Wait(CrowdyShardScheduling::ShardWaitMilliseconds(RemainingSeconds));
}

void UCrowdyActorTracker::ReportSchedulingUnavailable()
{
	if (bReportedSchedulingUnavailable.exchange(true, std::memory_order_acq_rel))
		return;

	UE_LOG(LogCrowdyReplication, Warning,
		TEXT("[Crowdy Actor Tracker]: The worker pool is not accepting work, so remote actors will stop updating. Reported once."));
}

void UCrowdyActorTracker::ReportStaleQueuedUpdateDropped()
{
	if (bReportedStaleQueuedUpdateDropped.exchange(true, std::memory_order_acq_rel))
		return;

	UE_LOG(LogCrowdyReplication, Verbose,
		TEXT("[Crowdy Actor Tracker]: Dropped a queued update for an actor that finished appearing while the "
			"update waited. It can no longer be placed in order against the updates now arriving for that "
			"actor, so the actor holds its spawn position until the next one. Reported once."));
}

void UCrowdyActorTracker::ReportTrackingAtCapacity()
{
	if (bReportedTrackingAtCapacity.exchange(true, std::memory_order_acq_rel))
		return;

	UE_LOG(LogCrowdyReplication, Warning,
		TEXT("[Crowdy Actor Tracker]: Tracking %d remote actors, which is the configured Max Tracked Actors. Any beyond that will not appear until one times out. Reported once."),
		MaxTrackedActors);
}

bool UCrowdyActorTracker::LoadDeveloperSettings()
{
	// ResolveProfileForWorld reports a map that resolves nothing, once for the whole world, naming which of
	// its causes it is. Repeating it here would say less than that line does and say it again for every
	// subsystem that asks.
	const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(GetWorld());
	if (!Profile)
		return false;

	const FCrowdyActorManagementConfigStruct& Config = Profile->ActorManagement;
	if (!Config.bUseCrowdyActorTracker)
		return false;

	Configure(
		Config.MaxTrackedActors,
		Config.MaxUpdatesPerBatch,
		Config.MaxBatchWaitTime,
		Config.ActorTimeoutThreshold
	);

	ToggleOwnerTracking(Config.bEnableOwnerTracking);
	ToggleBroadcastUpdatesToGameThread(Config.bDispatchUpdatesOnGameThread);

	// Stated once per world rather than left to be read out of a data asset. The value that governs is
	// whatever the map profile serialized, which is not necessarily the default this build ships, and a
	// crowd that stops growing at a number nobody chose is otherwise read as a rendering limit.
	UE_LOG(LogCrowdyReplication, Log,
		TEXT("[Crowdy Actor Tracker]: tracking at most %d remote actors, timing one out after %.1f s, from map "
			"profile '%s'."),
		MaxTrackedActors, ActorTimeoutThreshold, *GetNameSafe(Profile));

	return true;
}

void UCrowdyActorTracker::ProcessQueue(int32 ShardIndex)
{
	// Checked here and again inside the gather loop, since a batch can be waiting when shutdown starts. What is
	// dispatched below reaches actors and delegates that go away with the world, so a batch gathered from here on
	// has nowhere to be delivered.
	if (bShuttingDown.load(std::memory_order_acquire))
		return;

	TArray<FCrowdyActorUpdate> Batch;
	Batch.Reserve(MaxUpdatesPerBatch);

	const double StartTime = FPlatformTime::Seconds();

	while (true)
	{
		FCrowdyActorUpdate Update;

		while (Batch.Num() < MaxUpdatesPerBatch && UpdateQueues[ShardIndex]->Dequeue(Update))
			Batch.Add(MoveTemp(Update));

		if (bShuttingDown.load(std::memory_order_acquire))
			return;

		const double Remaining = MaxBatchWaitTime - (FPlatformTime::Seconds() - StartTime);

		const CrowdyShardScheduling::EBatchStep Step =
			CrowdyShardScheduling::DecideBatchStep(Batch.Num(), MaxUpdatesPerBatch, Remaining);

		if (Step == CrowdyShardScheduling::EBatchStep::Dispatch) break;
		if (Step == CrowdyShardScheduling::EBatchStep::Abandon) return;

		WaitForShardWork(ShardIndex, Remaining);
	}

    if (Batch.IsEmpty())
        return;

	
    TArray<const FCrowdyActorUpdate*, TInlineAllocator<16>> NewUpdates;

    for (const FCrowdyActorUpdate& Update : Batch)
    {
        const CrowdyActorUpdateRouting::EQueuedVerdict Verdict = CrowdyActorUpdateRouting::JudgeQueuedUpdate(
            TrackedUUIDs->Contains(Update.UUID),
            PendingSpawns->Contains(Update.UUID));

        if (Verdict == CrowdyActorUpdateRouting::EQueuedVerdict::Spawn)
            NewUpdates.Add(&Update);
        else if (Verdict == CrowdyActorUpdateRouting::EQueuedVerdict::DropStale)
            ReportStaleQueuedUpdateDropped();
    }

    if (!NewUpdates.IsEmpty())
    {
        TArray<FCrowdyActorUpdate> SpawnBatch;
        SpawnBatch.Reserve(NewUpdates.Num());
        
        for (const FCrowdyActorUpdate* Update : NewUpdates)
        {
            const FCrowdyGuidSet::EAddResult Result = PendingSpawns->Add(Update->UUID);

            if (Result == FCrowdyGuidSet::EAddResult::Added)
                SpawnBatch.Add(*Update);
            else if (Result == FCrowdyGuidSet::EAddResult::AtCapacity)
                ReportTrackingAtCapacity();
        }
        
        if (!SpawnBatch.IsEmpty())
        {
        	// Capture batch by value - safe to read from GT. The tracker itself is held weakly, since this runs
        	// after the consumer that posted it has finished and can arrive once the world has torn down.
        	AsyncTask(ENamedThreads::GameThread,
				[WeakThis = TWeakObjectPtr<UCrowdyActorTracker>(this), SpawnBatch = MoveTemp(SpawnBatch)]()
				{
					// Now truly on the game thread - GC locked, UObjects safe
					UCrowdyActorTracker* Tracker = WeakThis.Get();
					if (!Tracker)
						return;

					// Recorded before the broadcast, because a listener resolves the entity's class during
					// it. Done here rather than per update: the class an entity claims cannot change
					// while it is tracked, so an appearance is the only moment it needs writing.
					{
						FWriteScopeLock W(Tracker->ClassIDLock);
						for (const FCrowdyActorUpdate& Update : SpawnBatch)
						{
							Tracker->ClassIDByUUID.Add(Update.UUID, static_cast<FCrowdyClassID>(Update.ClassID));
						}
					}

					for (const FCrowdyActorUpdate& Update : SpawnBatch)
					{
						const int32 Count = ++Tracker->NumOfTrackedActors;

						// Through ResolveState, because the spawn path carries the retained message like every
						// other path now. The delegate takes the struct by value and copies it, which is once
						// per entity appearing rather than once per update.
						Tracker->OnRemoteEntityAppeared.Broadcast(Update.UUID, Update.ResolveState(), Count + 1);
					}

					for (const FCrowdyActorUpdate& Update : SpawnBatch)
					{
						Tracker->PendingSpawns->Remove(Update.UUID);

						// An actor that was spawned but could not be recorded here reads as new on its very next
						// update, and is spawned again, and again. Worth saying out loud rather than leaving as a
						// stream of duplicates with no stated cause.
						if (Tracker->TrackedUUIDs->Add(Update.UUID) == FCrowdyGuidSet::EAddResult::AtCapacity)
							Tracker->ReportTrackingAtCapacity();
					}
				}
			);
        }
    }
	
    {
        const double Now = FPlatformTime::Seconds();
        FWriteScopeLock W(LastUpdateLock);
        for (const FCrowdyActorUpdate& Update : Batch)
            LastUpdateTimes.Add(Update.UUID, Now);
    }
	
}

void UCrowdyActorTracker::CheckTimeouts()
{
	const double Now = FPlatformTime::Seconds();
	TArray<FGuid> TimedOut;

	{
		FReadScopeLock R(LastUpdateLock);
		for (const auto& Pair : LastUpdateTimes)
			if (Now - Pair.Value > ActorTimeoutThreshold)
				TimedOut.Add(Pair.Key);
	}

	if (TimedOut.IsEmpty())
		return;

	{
		FWriteScopeLock W(LastUpdateLock);
		for (const FGuid& UUID : TimedOut)
			LastUpdateTimes.Remove(UUID);
	}

	ProcessTimedOutActors(MoveTemp(TimedOut));
}

FCrowdyClassID UCrowdyActorTracker::GetClassIDForUUID(const FGuid& UUID) const
{
	FReadScopeLock R(ClassIDLock);
	const FCrowdyClassID* Found = ClassIDByUUID.Find(UUID);
	return Found ? *Found : CROWDY_INVALID_CLASS_ID;
}

void UCrowdyActorTracker::ProcessTimedOutActors(TArray<FGuid> TimedOut)
{
	{
		FWriteScopeLock W(ClassIDLock);
		for (const FGuid& UUID : TimedOut)
		{
			ClassIDByUUID.Remove(UUID);
		}
	}

	for (const FGuid& UUID : TimedOut)
	{
		TrackedUUIDs->Remove(UUID);
		PendingSpawns->Remove(UUID);
		--NumOfTrackedActors;
	}

	const int32 Count = NumOfTrackedActors.load();

	// Held weakly for the same reason as the other hops onto the game thread: this is posted from wherever the
	// timeout check ran and is delivered later, so the tracker it belongs to may be gone by then.
	FFunctionGraphTask::CreateAndDispatchWhenReady(
		[WeakThis = TWeakObjectPtr<UCrowdyActorTracker>(this), TimedOut = MoveTemp(TimedOut), Count]()
		{
			check(IsInGameThread());

			UCrowdyActorTracker* Tracker = WeakThis.Get();
			if (!Tracker)
				return;

			for (const FGuid& UUID : TimedOut)
				Tracker->OnRemoteEntityTimedOut.Broadcast(UUID, Count + 1);
		},
		TStatId{},
		nullptr,
		ENamedThreads::GameThread
	);
}

void UCrowdyActorTracker::SetLocalUUID(FString NewUUID)
{
	LocalUUID = USerializationFunctionLibrary::ToGuid(NewUUID);
	
	if (!LocalUUID.IsValid())
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Tracker]: Invalid local UUID: %s"), *NewUUID);
	
}

