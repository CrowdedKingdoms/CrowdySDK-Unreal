// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyRenderingBackend.h"
#include "CrowdyActorPoolBackend.generated.h"

class UCrowdyActorPoolSubsystem;
class UCrowdyRepApplicationPolicy;
class UCrowdyEntitySubsystem;
class UCrowdyActorPoolBackendConfig;

/**
 * Default first-party rendering backend. Uses UCrowdyActorPoolSubsystem to manage
 * a pool of pre-spawned actors per entity class, delegates per-frame state application
 * to a UCrowdyRepApplicationPolicy, and registers each active actor in UCrowdyEntitySubsystem
 * so entity-targeted events reach pool actors the same way they reach static entities.
 * Configure via UCrowdyActorPoolBackendConfig.
 */
UCLASS(Blueprintable, BlueprintType, EditInlineNew, DefaultToInstanced,
	meta=(DisplayName="Actor Pool Rendering Backend"))
class CROWDYREPLICATION_API UCrowdyActorPoolBackend : public UCrowdyRenderingBackend
{
	GENERATED_BODY()

public:

	virtual void InitializeBackend(UWorld* World, UCrowdyRenderingBackendConfig* Config) override;
	virtual void DeinitializeBackend() override;

	virtual void ActivateInstance(int32 SlotId, const FGuid& UUID, UClass* EntityClass, const FInstancedStruct& InitialState) override;
	virtual void DeactivateInstance(int32 SlotId, const FGuid& UUID) override;
	virtual void ExtractUpdate(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId) override;
	virtual void ApplyInterpolation(int32 SlotId, int64 RenderTimeMs) override;

private:

	UPROPERTY()
	TObjectPtr<UCrowdyActorPoolSubsystem> ActorPool;

	UPROPERTY()
	TObjectPtr<UCrowdyRepApplicationPolicy> Policy;

	UPROPERTY()
	TObjectPtr<UCrowdyEntitySubsystem> EntitySubsystem;

	// Cached config pointer valid for the lifetime of the backend.
	UPROPERTY()
	TObjectPtr<UCrowdyActorPoolBackendConfig> PoolConfig;

	/** Per-slot actor handle, parallel to CrowdyActorManager's Slots array. */
	TArray<TWeakObjectPtr<AActor>> SlotActors;

	void EnsureSlotCapacity(int32 SlotId);
	void EnsurePoolForClass(UClass* ActorClass);
};
