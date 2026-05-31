// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyAutoReplicator.generated.h"


class UCrowdyActorUpdateComponent;
class UCrowdySDKSubsystem;

struct FReplicationData
{
	// Hot path: only what's needed per-frame
	TArray<FVector3f> Positions;
	TArray<FInt64Vector> Chunks;
	TArray<FString> UUIDs;
	TArray<TWeakObjectPtr<UCrowdyActorUpdateComponent>> Components;
	int32 Count = 0;
};

/**
 * 
 */
UCLASS()
class CROWDYSDK_API UCrowdyAutoReplicator : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual TStatId GetStatId() const override;
	virtual void Tick(float DeltaTime) override;
	
	void RegisterReplicationComponent(UCrowdyActorUpdateComponent* Component);
	void UnregisterReplicationComponent(UCrowdyActorUpdateComponent* Component);
	
private:
	
	UPROPERTY()
	UCrowdySDKSubsystem* CrowdySDK;

	FReplicationData Data;
	
	float ReplicationInterval = 0.1f;
	float ReplicationAccumulator = 0.f;
	
	bool bIsTicking = false;

private:
	void ReplicationLoop();
};
