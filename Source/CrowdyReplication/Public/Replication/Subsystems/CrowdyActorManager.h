// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyActorManager.generated.h"

class UCrowdyActorTracker;
class UCrowdyRenderingBackend;
class UCrowdyEntitySubsystem;
struct FCrowdyActorUpdate;

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

	void RegisterStateClass(UScriptStruct* Struct, TSubclassOf<AActor> ActorClass);

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

	// Network updates arrived before the entity's spawn event was processed.
	// Keyed by UUID; fired when EntitySubsystem's OnEntityRegistered broadcasts.
	struct FPendingActivation
	{
		int32 SlotId = INDEX_NONE;
		FInstancedStruct InitialState;
	};

	TArray<FSlotEntry> Slots;
	TMap<FGuid, int32> UUIDToSlot;
	TMap<FGuid, FPendingActivation> PendingActivations;
	TQueue<FCrowdyActorUpdate, EQueueMode::Mpsc> UpdateQueue;

	// Auto-populated at BeginPlay by each UCrowdyEntityComponent via RegisterStateClass.
	// Maps update payload struct type → pool actor class for dynamic entities.
	TMap<const UScriptStruct*, TSubclassOf<AActor>> StateClassMap;

private:

	void UpdateServerTimeOffset(int64 ServerTimestampMs);
	int64 GetEstimatedServerTimeMs() const;

	void ApplyPendingUpdates();
	void TickInterpolation();

	int32 AllocateSlot(const FGuid& UUID);
	void FreeSlot(const FGuid& UUID);

	bool LoadConfig();

	// State struct map is the primary path for dynamic entities (no spawn event needed).
	// Falls back to EntitySubsystem for entities registered via spawn event.
	UClass* ResolveEntityClass(const FGuid& UUID, const FInstancedStruct& State) const;

	UFUNCTION()
	void HandleActorSpawned(FGuid UUID, FInstancedStruct InitialState, int32 ActorCount);

	UFUNCTION()
	void HandleActorDestroyed(FGuid UUID, int32 ActorCount);

	UFUNCTION()
	void HandleUpdateBatch(const TArray<FCrowdyActorUpdate>& Updates);

	UFUNCTION()
	void OnEntityRegistered(const FGuid& EntityID);
};
