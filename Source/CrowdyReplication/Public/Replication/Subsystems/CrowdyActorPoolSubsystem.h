// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyActorPoolPolicy.h"
#include "Data/FCrowdyPoolConfig.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyActorPoolSubsystem.generated.h"

USTRUCT()
struct FSlot
{
	GENERATED_BODY()

	TWeakObjectPtr<AActor> Actor;
	bool bActive = false;
};


USTRUCT()
struct FPool
{
	GENERATED_BODY()

	TSubclassOf<AActor> ActorClass;

	UPROPERTY()
	TObjectPtr<UCrowdyActorPoolPolicy> Policy;

	TArray<FSlot> Slots;

	int32 MaxSize = 0;
	bool bWarnedAtCap = false;
};

/**
 * Generic actor pool: pre-spawns actors, checks them out/in by pointer, and delegates
 * visibility to UCrowdyActorPoolPolicy. Has no knowledge of entity IDs or subsystems:
 * callers own the UUID→actor mapping.
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Actor Pool Subsystem"))
class CROWDYREPLICATION_API UCrowdyActorPoolSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/**
	 * Register a pool for the given actor class. Spawns PoolSize actors immediately.
	 * Calling this more than once for the same class is a no-op.
	 */
	UFUNCTION(BlueprintCallable, Category="Actor Pool")
	void RegisterPool(const FCrowdyPoolConfig& Config);

	/**
	 * Check out an inactive actor. Calls OnActorActivated on the pool policy. When every actor is in use the pool
	 * grows by one, up to the config's MaxPoolSize; returns null only at that cap.
	 */
	AActor* AcquireActor(TSubclassOf<AActor> ActorClass);

	/** Return an actor to the pool by pointer. Calls OnActorDeactivated on the pool policy. */
	void ReleaseActor(AActor* Actor);

	/** Whether RegisterPool has already run for exactly this class. */
	bool HasPool(const UClass* ActorClass) const;

private:

	UPROPERTY()
	TMap<TSubclassOf<AActor>, FPool> Pools;

	FPool* FindPool(const UClass* ActorClass);

	/** Spawns one dormant, pooled, inactive actor into Pool. Null if the spawn failed. */
	AActor* SpawnPooledActor(FPool& Pool);
};
