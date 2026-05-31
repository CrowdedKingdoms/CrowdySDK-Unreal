// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "CrowdyActorPoolPolicy.generated.h"

/**
 * 
 */
UCLASS(Abstract, Blueprintable, BlueprintType)
class CROWDYSDK_API UCrowdyActorPoolPolicy : public UObject
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
