// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "ActorUpdateExecutor.generated.h"

class UCrowdyActorUpdateComponent;
/**
 * 
 */
UCLASS(Blueprintable, BlueprintType, EditInlineNew, Abstract)
class CROWDYSDK_API UActorUpdateExecutor : public UObject
{
	GENERATED_BODY()
public:
	
	UFUNCTION(BlueprintNativeEvent, Category = "Crowdy SDK|Actor Update Executor")
	FInstancedStruct GetActorState(const UCrowdyActorUpdateComponent* UpdateComponent) const;
};
