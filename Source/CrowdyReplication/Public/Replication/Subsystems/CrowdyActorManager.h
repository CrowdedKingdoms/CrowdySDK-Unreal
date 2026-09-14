// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyActorManager.generated.h"

class UCrowdyRenderingBackend;
class UCrowdyEntitySubsystem;

/**
 *
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Actor Manager"))
class CROWDYREPLICATION_API UCrowdyActorManager : public UTickableWorldSubsystem
{
	GENERATED_BODY()
public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdyActorManager, STATGROUP_Tickables); }

	/**
	 * Swap the active rendering backend at runtime.
	 * The previous backend is deinitialized; the new one must already be initialized.
	 * Current slot state is preserved; the new backend receives ApplyInterpolation from
	 * the next tick onward.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Crowdy Actor Manager")
	void SetBackend(UCrowdyRenderingBackend* NewBackend);

	// The live backend, null when none initialized; a backend that refused to initialize is absent here and present in the profile.
	UCrowdyRenderingBackend* GetActiveBackend() const { return ActiveBackend; }

	void RegisterStateClass(UScriptStruct* Struct, TSubclassOf<AActor> ActorClass);

	// A network update that arrived before the entity's spawn event was processed. Keyed by UUID;
	// activation completes when the entity subsystem's OnEntityRegistered broadcasts.
	struct FPendingActivation
	{
		int32 SlotId = INDEX_NONE;
		FInstancedStruct InitialState;

		// Ticks spent waiting for a spawn event that resolves this entity's class. A late spawn event
		// still completes activation normally; the count only drives the stranded report and eviction.
		int32 TicksWaiting = 0;
		bool bWarnedStranded = false;
	};

	/**
	 * Ages the pending-activation park by one tick and decides what leaves it.
	 *
	 * An entry that reaches WarnTicks is reported once and keeps its place, since a genuinely late
	 * spawn event can still complete it. An entry that reaches EvictTicks is reported and removed,
	 * and its UUID is appended to OutEvicted so the caller can release the render slot it was holding.
	 * Without that second bound an entry whose class is never resolvable holds its slot for the life
	 * of the world.
	 *
	 * The two bounds are arguments rather than being read here, so this stays a plain function of the
	 * park's contents.
	 */
	static void AgePendingActivations(TMap<FGuid, FPendingActivation>& Park, int32 WarnTicks, int32 EvictTicks, TArray<FGuid>& OutEvicted);

#if WITH_DEV_AUTOMATION_TESTS
	// Reaches the slot and config plumbing without a subsystem collection behind it. Standing up a real
	// world subsystem brings up every other game world subsystem in the project, some of which assert
	// outside a running game.
	bool LoadConfigForTest() { return LoadConfig(); }

	// The production binding, so a case about which departure releases a slot exercises the wiring the game gets
	// rather than a copy of it made in the test. A test that bound the delegates itself would keep passing with
	// the binding deleted from Initialize.
	void BindToTrackerForTest(UCrowdyActorTracker* Tracker) { BindToTracker(Tracker); }

	int32 AllocateSlotForTest(const FGuid& UUID) { return AllocateSlot(UUID); }
	void ReleaseSlotForTest(const FGuid& UUID) { ReleaseSlot(UUID); }
	void TickPendingActivationsForTest() { TickPendingActivations(); }

	// So a test can tick past the eviction bound without naming a number of its own. A test that picks its own
	// figure passes or fails on how the bound was last tuned rather than on the behaviour it is checking.
	static int32 GetPendingActivationEvictTicksForTest();

	// The real queue and the real drain, so a test sees what the backend is handed rather than what a
	// caller passed straight to it. ExtractUpdateForTest below deliberately bypasses both, so it cannot
	// see which of an update's two state carriers the drain reads.
	void DrainUpdateBatchForTest(const TArray<FCrowdyActorUpdate>& Updates)
	{
		HandleUpdateBatch(Updates);
		ApplyPendingUpdates();
	}

	// Puts an update in through the cross-thread queue rather than the game-thread array. The drain reads
	// the two in a fixed order and only a case that can fill both is able to pin which.
	void EnqueueOffThreadForTest(const FCrowdyActorUpdate& Update) { UpdateQueue.Enqueue(Update); }

	// Parks an entry holding a slot, which is the state an update that arrives before its spawn event puts
	// the manager into.
	void ParkActivationForTest(const FGuid& UUID);

	// Feeds one update to whatever slot the UUID currently holds, parked or activated, the way
	// ApplyPendingUpdates does.
	void ExtractUpdateForTest(const FGuid& UUID, const FInstancedStruct& State);
#endif

private:

	UPROPERTY()
	TObjectPtr<UCrowdyActorTracker> ActorTracker;

	UPROPERTY()
	TObjectPtr<UCrowdyRenderingBackend> ActiveBackend;

	UPROPERTY()
	TObjectPtr<UCrowdyEntitySubsystem> EntitySubsystem;

	int64 ServerTimeOffsetMs = 0;
	int64 BestOffsetMs = 0;
	bool bHasInitialOffset = false;

	UPROPERTY(EditDefaultsOnly, Category="Crowdy SDK|Crowdy Actor Manager")
	int64 InterpolationDelayMs = 100;

	struct FSlotEntry
	{
		FGuid UUID;
		bool bActive = false;
	};

	TArray<FSlotEntry> Slots;
	TMap<FGuid, int32> UUIDToSlot;
	TMap<FGuid, FPendingActivation> PendingActivations;

	// Handed over on the game thread, so it needs no queue and no node per update. Reset rather than
	// emptied, so a frame's capacity is paid for once.
	TArray<FCrowdyActorUpdate> PendingUpdates;

	// Only for a batch broadcast from another thread, which the tracker no longer does but the delegate
	// still permits.
	TQueue<FCrowdyActorUpdate, EQueueMode::Mpsc> UpdateQueue;

	// Auto-populated at BeginPlay by each UCrowdyEntityComponent via RegisterStateClass.
	// Maps update payload struct type → pool actor class for dynamic entities.
	TMap<const UScriptStruct*, TSubclassOf<AActor>> StateClassMap;

private:

	/** Takes the clock reading rather than sampling it, so one drain's worth of updates share one sample. */
	void UpdateServerTimeOffset(int64 ServerTimestampMs, int64 ClientNowMs);
	int64 GetEstimatedServerTimeMs() const;

	void ApplyPendingUpdates();
	void TickInterpolation();
	void TickPendingActivations();

	/** Subscribe to everything the tracker reports. The one place those delegates are bound. */
	void BindToTracker(UCrowdyActorTracker* Tracker);

	int32 AllocateSlot(const FGuid& UUID);

	/**
	 * Gives a slot back for reuse, after telling the backend to clean up whatever it holds for it.
	 *
	 * Every path that hands a slot back goes through here, including the ones where the instance was never
	 * activated: a slot is reachable from UUIDToSlot from the moment it is allocated, so it can have received
	 * ExtractUpdate calls whether or not ActivateInstance was ever called for it.
	 */
	void ReleaseSlot(const FGuid& UUID);

	/** Marks the slot reusable. Callers want ReleaseSlot; this leaves the backend's per-slot state alone. */
	void FreeSlot(const FGuid& UUID);

	bool LoadConfig();

	// State struct map is the primary path for dynamic entities (no spawn event needed).
	// Falls back to EntitySubsystem for entities registered via spawn event.
	UClass* ResolveEntityClass(const FGuid& UUID, const FInstancedStruct& State) const;

	UFUNCTION()
	void HandleActorSpawned(FGuid UUID, FInstancedStruct InitialState, int32 ActorCount);

	UFUNCTION()
	void HandleActorDestroyed(FGuid UUID, int32 ActorCount);

	// The server announcing a departure releases the actor exactly as a staleness timeout does. Both have to
	// arrive here: a leave removes the entity from the map the timeout check reads, so an actor reported gone
	// by the server would otherwise never be released by anything.
	UFUNCTION()
	void HandleActorLeft(const FCrowdyActorLeft& ActorLeft, int32 ActorCount);

	UFUNCTION()
	void HandleUpdateBatch(const TArray<FCrowdyActorUpdate>& Updates);

	UFUNCTION()
	void OnEntityRegistered(const FGuid& EntityID);
};
