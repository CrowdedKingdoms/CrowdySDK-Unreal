// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/SharedPointer.h"
#include "Data/FCrowdyGuidSet.h"
#include "Replication/Subsystems/CrowdyShardScheduling.h"

#include <atomic>

#include "CrowdyActorTracker.generated.h"


class FEvent;
class UCrowdyWorkerThreadsSubsystem;
struct FActorUpdateNotificationMessage;
struct FCrowdyDelivery;

// Update Packet
USTRUCT(BlueprintType)
struct FCrowdyActorUpdate
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Actor Update")
	FGuid UUID;

	UPROPERTY(BlueprintReadWrite)
	FInstancedStruct State;

	/**
	 * The message this update was decoded from, retained so the state is read where it was decoded instead of
	 * copied into every update. Empty on anything that reached a Blueprint, which carries State instead.
	 *
	 * Deliberately not a UPROPERTY, and it cannot become one: the game-thread delegate below is dynamic and
	 * marshals property by property, so this would be dropped in transit rather than diagnosed. Read through
	 * ResolveState, never directly, and see it for which of the two is set on which path.
	 */
	TSharedPtr<const FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message;

	/**
	 * The decoded state, from whichever of the two above is carrying it.
	 *
	 * Retaining the message keeps its decoded payload, which owns its storage. It does NOT keep the frame's
	 * octets, so nothing reached through here may read the message's views.
	 */
	CROWDYREPLICATION_API const FInstancedStruct& ResolveState() const;

	UPROPERTY(BlueprintReadWrite)
	int64 ServerTimestamp = 0;

	// The entity class the sender named on the wire. Widened to int64 because the wire id is a uint32,
	// which UHT cannot expose, and this struct crosses a dynamic delegate whose marshalling copies
	// property by property: a plain C++ member here would be silently dropped in transit.
	UPROPERTY(BlueprintReadWrite)
	int64 ClassID = 0;
};



DECLARE_TS_MULTICAST_DELEGATE_OneParam(FOnTrackedActorUpdateBatch, const TArray<FCrowdyActorUpdate>&);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnActorUpdateGameThreadBatch, const TArray<FCrowdyActorUpdate>&, Updates);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnActorSpawnRequested, FGuid, UUID, FInstancedStruct, IntialState, int32, ActorCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnActorTimeoutRequested,FGuid, UUID, int32, ActorCount);



/**
 * 
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Actor Tracker"))
class CROWDYREPLICATION_API UCrowdyActorTracker : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdyActorTracker, STATGROUP_Tickables); }

public:

	/**
	 * Updates for actors already on screen, handed on once a frame.
	 *
	 * Fires on the game thread. It used to fire on a shard worker, which is why it was named for one; an
	 * actor already being tracked is now routed straight from the receive path and never crosses a thread.
	 * C++ only.
	 */
	FOnTrackedActorUpdateBatch OnTrackedActorUpdates;

	//BP Path (Game Thread, opt-in)
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Actor Tracker|Events")
	FOnActorUpdateGameThreadBatch OnUpdatesGameThread;
	
	// Spawn Delegate
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Actor Tracker|Events")
	FOnActorSpawnRequested OnRemoteEntityAppeared;

	// Destroy/Despawn delegate
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Actor Tracker|Events")
	FOnActorTimeoutRequested OnRemoteEntityTimedOut;

public:
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Actor Tracker")
	void ToggleOwnerTracking(const bool bEnable);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Actor Tracker")
	void ToggleBroadcastUpdatesToGameThread(const bool bEnable);
	
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Actor Tracker")
	void Configure(int32 InMaxTrackedActors,
		int32 InMaxUpdatesPerBatch,
		float InMaxBatchWaitTime,
		float InActorTimeoutThreshold);

	/**
	 * The entity class a tracked UUID declared on the wire, or CROWDY_INVALID_CLASS_ID if it is not
	 * tracked. C++ only: the id is a uint32 wire identity, which is neither resolvable by UHT nor a
	 * Blueprint property type.
	 */
	FCrowdyClassID GetClassIDForUUID(const FGuid& UUID) const;

private:

	// The tracker is the single, unfiltered consumer of actor updates; released on Deinitialize.
	FCrowdySubscription ActorUpdateSubscription;

	void HandleActorUpdateDelivery(const FCrowdyDelivery& Delivery);

	// What is already on screen, and what is on its way there. Read by every shard consumer and written by the game
	// thread, so both are shared across threads. Whether an inbound update is an existing actor or a new one is
	// decided by asking these two in order, which is why a wrong answer from either spawns a duplicate.
	TUniquePtr<FCrowdyGuidSet> TrackedUUIDs;
	TUniquePtr<FCrowdyGuidSet> PendingSpawns;
	
	// Updates for actors already on screen, gathered where they are received and handed on once a frame. They
	// never enter the shard queues below, so an actor's updates cannot be reordered against one another and
	// cost no queue node. Reset rather than emptied, so the frame's capacity is paid for once.
	TArray<FCrowdyActorUpdate> TrackedUpdates;

	// Inbound updates for an actor NOT yet on screen are spread across shards by actor, so one actor's updates
	// always land on the same queue and stay in order. Each shard is read by one consumer at a time; the state
	// is what a consumer has to win, and the event is how a consumer waiting for its batch to fill is told that
	// something arrived. The events are owned here rather than borrowed from the shared pool, so one that a
	// consumer might still be waiting on can safely be abandoned at shutdown.
	TArray<TUniquePtr<TQueue<FCrowdyActorUpdate, EQueueMode::Mpsc>>> UpdateQueues;
	TArray<CrowdyShardScheduling::FShardState> ShardStates;
	TArray<FEvent*> ShardEvents;
	int32 NumOfShards = 1;

	// Consumers already handed to the pool. Shutdown waits for these, because they read state that is about to
	// be destroyed and because a queue may only be drained once nothing else is reading it. It counts consumers
	// only: work a consumer posts on to the game thread is not covered, and holds this object weakly instead.
	std::atomic<int32> ShardTasksInFlight { 0 };
	std::atomic<bool> bShuttingDown { false };
	std::atomic<bool> bReportedSchedulingUnavailable { false };
	std::atomic<bool> bReportedTrackingAtCapacity { false };
	std::atomic<bool> bReportedGatheredBacklogDiscarded { false };
	std::atomic<bool> bReportedStaleQueuedUpdateDropped { false };
	
	//Timeout
	TMap<FGuid, double> LastUpdateTimes;
	FRWLock LastUpdateLock;
	FTimerHandle TimeoutTimerHandle;

	// The entity class each tracked UUID declared on the wire, so the actor manager can resolve a class
	// from the message rather than inferring one from the state struct, where two classes sharing a
	// struct collapse into whichever registered last.
	//
	// Written when an entity first appears and erased when it times out, never per update: the class an
	// entity claims does not change while it is tracked, and LastUpdateTimes above is already the one
	// map this path writes on every update.
	TMap<FGuid, FCrowdyClassID> ClassIDByUUID;
	mutable FRWLock ClassIDLock;
	
	//Config
	int32 MaxTrackedActors = 1024;
	int32 MaxUpdatesPerBatch = 64;
	float MaxBatchWaitTime = 0.002f;
	float ActorTimeoutThreshold = 5.0f;
	
	//State
	FGuid LocalUUID;
	bool bOwnerTrackingEnabled = true;
	bool bBroadcastUpdatesToGameThread = false;
	
	UPROPERTY()
	TObjectPtr<UCrowdyWorkerThreadsSubsystem> WorkerThreadsSubsystem;

	std::atomic<int32> NumOfTrackedActors { 0 };

private:
	
	void SetupQueues();

	FORCEINLINE void EnqueueUpdate(const FCrowdyActorUpdate& Update);
	FORCEINLINE void EnqueueUpdate(FCrowdyActorUpdate&& Update);

	/**
	 * Hand on everything gathered for actors already on screen, and record that they were heard from.
	 *
	 * Done here rather than per update so the timeout map is written once a frame instead of once a
	 * message. Runs even with nothing subscribed, because the timeout bookkeeping is not optional.
	 */
	void FlushTrackedUpdates();

	/** The ceiling on gathered updates, past which the backlog is discarded rather than grown. */
	static int32 GetMaxGatheredUpdates();

	/**
	 * Throw away gathered updates nothing has consumed, keeping the actors alive.
	 *
	 * Only reachable while the world tick is not running and the receive path is, which the core ticker
	 * makes possible. What is discarded is superseded view state, and every actor is still recorded as
	 * having been heard from so none of them times out over it.
	 */
	void DiscardGatheredBacklog();
	
	bool LoadDeveloperSettings();
	void ProcessQueue(int32 ShardIndex);

	/** Hand one shard to the worker pool. Answers whether the pool took it, so the shard can be released if not. */
	bool ScheduleShardTask(int32 ShardIndex);

	/**
	 * Block until no consumer is reading a shard, so the queues can be drained.
	 *
	 * Bounded, so it answers whether that is actually true rather than waiting forever for a consumer that can no
	 * longer run. It covers shard consumers and nothing else: work one of them posts on to the game thread runs
	 * after it has finished and is not waited for here.
	 */
	bool WaitForShardTasks();

	/** Wait for work to arrive on a shard, for at most the time its batch window has left. */
	void WaitForShardWork(int32 ShardIndex, double RemainingSeconds);

	/** Say once that updates are not being processed. Every update would otherwise report the same thing. */
	void ReportSchedulingUnavailable();

	/**
	 * Say once that the tracked actor ceiling has been reached, so remote actors beyond it are being ignored.
	 *
	 * Worth its own report because the symptom does not look like a limit: the actors already present carry on
	 * updating normally and the new ones simply never appear.
	 */
	void ReportTrackingAtCapacity();

	/**
	 * Say once that an update was dropped for arriving before its actor appeared and being read after.
	 *
	 * Worth saying at all because the update is genuinely lost rather than superseded, and worth saying only
	 * once because it is bounded to the moment an actor appears and says nothing new on the next actor.
	 */
	void ReportStaleQueuedUpdateDropped();

	void CheckTimeouts();
	void ProcessTimedOutActors(TArray<FGuid> TimedOut);
	
	UFUNCTION()
	void SetLocalUUID(FString NewUUID);
	
};
