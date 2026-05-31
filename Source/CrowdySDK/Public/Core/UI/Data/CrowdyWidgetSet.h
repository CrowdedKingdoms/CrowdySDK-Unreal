// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Core/UI/Structures/FWidgetSpec.h"
#include "Engine/DataAsset.h"
#include "CrowdyWidgetSet.generated.h"

/**
 * 
 */
UCLASS(BlueprintType)
class CROWDYSDK_API UCrowdyWidgetSet : public UDataAsset
{
	GENERATED_BODY()
	
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(DisplayName="Widget Specifications"))
	TMap<FGameplayTag, FWidgetSpec> WidgetSpecMap;
};
