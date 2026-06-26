// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "CrowdyActorPoolPolicy.generated.h"

/**
 * Default actor-pool policy: hides pooled actors, shows them on activation, and strips
 * proxy movement. It is concrete and fully functional, so UCrowdyActorPoolBackendConfig
 * may leave PoolPolicyClass empty — the pool subsystem falls back to this class. Subclass
 * it only to add per-actor activate/deactivate behavior.
 */
UCLASS(Blueprintable, BlueprintType)
class CROWDYREPLICATION_API UCrowdyActorPoolPolicy : public UObject
{
	GENERATED_BODY()
public:
	
	/** Called once per actor right after it's spawned into the pool. */
	UFUNCTION(BlueprintNativeEvent, Category="Pool Policy")
	void OnActorPooled(AActor* Actor);
	
	/** Called when an actor is checked out and assigned a UUID. */
	UFUNCTION(BlueprintNativeEvent, Category="Pool Policy")
	void OnActorActivated(AActor* Actor, const FInstancedStruct& InitialState);
	
	/** Called when an actor is returned to the pool. */
	UFUNCTION(BlueprintNativeEvent, Category="Pool Policy")
	void OnActorDeactivated(AActor* Actor);
};
