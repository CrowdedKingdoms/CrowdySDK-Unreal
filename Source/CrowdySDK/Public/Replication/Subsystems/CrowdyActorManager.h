// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyActorManager.generated.h"

class UCrowdyActorPoolSubsystem;
class UCrowdyActorTracker;
struct FCrowdyActorUpdate;
class UCrowdyRepApplicationPolicy;

/**
 * 
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Actor Manager"))
class CROWDYSDK_API UCrowdyActorManager : public UTickableWorldSubsystem
{
	GENERATED_BODY()
public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdyActorManager, STATGROUP_Tickables); }
	
	
	/**
	 * Set the active policy. Call this at BeginPlay from wherever
	 * you configure your game mode / developer settings.
	 * Old policy is discarded; all current slots are cleared.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Crowdy Actor Manager")
	void SetPolicy(UCrowdyRepApplicationPolicy* NewPolicy);
	
private:
	
	UPROPERTY()
	UCrowdyActorTracker* ActorTracker;
	
	UPROPERTY()
	UCrowdyActorPoolSubsystem* ActorPool;
	
	UPROPERTY()
	TSubclassOf<AActor> ReplicatedActorClass;
	
	UPROPERTY()
	TObjectPtr<UCrowdyRepApplicationPolicy> ActivePolicy;
	
	int64 ServerTimeOffsetMs = 0;
	int64 BestOffsetMs = 0;
	bool bHasInitialOffset = false;
	
	UPROPERTY(EditDefaultsOnly, Category="Crowdy SDK|Crowdy Actor Manager")
	int64 InterpolationDelayMs = 100;
	
	struct FSlotEntry
	{
		TWeakObjectPtr<AActor> Actor;
		FGuid UUID;
		bool bActive = false;
	};
	
	TArray<FSlotEntry> Slots;
	TMap<FGuid, int32> UUIDToSlot;
	TQueue<FCrowdyActorUpdate, EQueueMode::Mpsc> UpdateQueue;
	TArray<int32> PendingRemovals;

private:
	
	void UpdateServerTimeOffset(int64 ServerTimestampMs);
	int64 GetEstimatedServerTimeMs() const;
	
	void ApplyPendingUpdates();
	void TickInterpolation();
	void ProcessRemovals();
	
	int32 AllocateSlot(const FGuid& UUID, AActor* Actor);
	void FreeSlot(const FGuid& UUID);
	
	bool LoadConfig();
	
	UFUNCTION()
	void HandleActorSpawned(FGuid UUID, FInstancedStruct InitialState, int32 ActorCount);
	
	UFUNCTION()
	void HandleActorDestroyed(FGuid UUID, int32 ActorCount);
	
	UFUNCTION()
	void HandleUpdateBatch(const TArray<FCrowdyActorUpdate>& Updates);
	
};
