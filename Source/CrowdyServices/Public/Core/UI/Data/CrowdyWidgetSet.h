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
class CROWDYSERVICES_API UCrowdyWidgetSet : public UDataAsset
{
	GENERATED_BODY()
	
public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|UI", meta=(DisplayName="Widget Specifications"))
	TMap<FGameplayTag, FWidgetSpec> WidgetSpecMap;
};
