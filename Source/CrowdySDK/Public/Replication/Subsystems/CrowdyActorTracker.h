// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "Data/FLockFreeGuidSet.h"
#include "CrowdyActorTracker.generated.h"


class UCrowdyWorkerThreadsSubsystem;

// Update Packet
USTRUCT(BlueprintType)
struct FCrowdyActorUpdate
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Actor Update")
	FGuid UUID;
	
	UPROPERTY(BlueprintReadWrite)
	FInstancedStruct State;
	
	UPROPERTY(BlueprintReadWrite)
	int64 ServerTimestamp = 0;
};



DECLARE_TS_MULTICAST_DELEGATE_OneParam(FOnActorUpdateWorkerBatch, const TArray<FCrowdyActorUpdate>&);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnActorUpdateGameThreadBatch, const TArray<FCrowdyActorUpdate>&, Updates);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnActorSpawnRequested, FGuid, UUID, FInstancedStruct, IntialState, int32, ActorCount);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnActorTimeoutRequested,FGuid, UUID, int32, ActorCount);



/**
 * 
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Actor Tracker"))
class CROWDYSDK_API UCrowdyActorTracker : public UWorldSubsystem, public ICrowdyReceptionLayer
{
	GENERATED_BODY()
	
public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) override;
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const override;
	virtual TArray<FName> GetSupportedActorUpdateTypes() const override;
	
public:
	
	//Fast Path (C++ only, worker thread)
	FOnActorUpdateWorkerBatch OnUpdatesWorkerThread;

	//BP Path (Game Thread, opt-in)
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Actor Tracker|Events")
	FOnActorUpdateGameThreadBatch OnUpdatesGameThread;
	
	// Spawn Delegate
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Actor Tracker|Events")
	FOnActorSpawnRequested OnNewPlayerJoined;

	// Destroy/Despawn delegate
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Actor Tracker|Events")
	FOnActorTimeoutRequested OnPlayerLeft;

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
	
private:
	
	// Lock Free UUID Tracking
	TUniquePtr<FLockFreeGuidSet> TrackedUUIDs;
	TUniquePtr<FLockFreeGuidSet> PendingSpawns;
	
	// Per-thread queues
	TArray<TUniquePtr<TQueue<FCrowdyActorUpdate, EQueueMode::Mpsc>>> UpdateQueues;
	TArray<std::atomic<bool>> WorkerScheduledFlags;
	int32 NumOfWorkerThread = 1;
	
	//Timeout
	TMap<FGuid, double> LastUpdateTimes;
	FRWLock LastUpdateLock;
	FTimerHandle TimeoutTimerHandle;
	
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
	
	UPROPERTY()
	TArray<FName> SupportedActorUpdateTypes;
	
	std::atomic<int32> NumOfTrackedActors { 0 };

private:
	
	void SetupQueues();
	
	FORCEINLINE void EnqueueUpdate(const FCrowdyActorUpdate& Update);
	FORCEINLINE void EnqueueUpdate(FCrowdyActorUpdate&& Update);
	
	bool LoadDeveloperSettings();
	void ProcessQueue(int32 WorkerIndex);
	void CheckTimeouts();
	void ProcessTimedOutActors(TArray<FGuid> TimedOut);
	
	UFUNCTION()
	void SetLocalUUID(FString NewUUID);
	
};
