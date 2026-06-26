// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyAutoReplicator.generated.h"


class ICrowdyReplicationSource;
class UCrowdySDKBridgeSubsystem;

struct FReplicationData
{
	// Hot path: only what's needed per-frame
	TArray<FVector3f> Positions;
	TArray<FInt64Vector> Chunks;
	TArray<FString> UUIDs;
	// Weak pointer guards lifetime; the raw interface pointer (resolved once at
	// registration) avoids a Cast<> per entry per replication tick.
	TArray<TWeakObjectPtr<const UActorComponent>> Components;
	TArray<const ICrowdyReplicationSource*> Sources;
	int32 Count = 0;
};

/**
 * 
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyAutoReplicator : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual TStatId GetStatId() const override;
	virtual void Tick(float DeltaTime) override;
	
	// Component must implement ICrowdyReplicationSource
	void RegisterReplicationComponent(UActorComponent* Component);
	void UnregisterReplicationComponent(UActorComponent* Component);
	
private:
	
	UCrowdySDKBridgeSubsystem* Bridge = nullptr;

	FReplicationData Data;
	
	float ReplicationInterval = 0.1f;
	float ReplicationAccumulator = 0.f;
	
	bool bIsTicking = false;

private:
	void ReplicationLoop();
};
