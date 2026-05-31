// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/Overlay.h"
#include "Core/UI/Enums/ECrowdyUILayer.h"
#include "UObject/Interface.h"
#include "CrowdyUIRootWidget.generated.h"

// This class does not need to be modified.
UINTERFACE(Blueprintable, BlueprintType)
class UCrowdyUIRootWidget : public UInterface
{
	GENERATED_BODY()
};

/**
 * 
 */
class CROWDYSDK_API ICrowdyUIRootWidget
{
	GENERATED_BODY()

public:
	
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|UI")
	UUserWidget* PushToLayer(ECrowdyUILayer Layer,  bool bClearLayerFirst = false, TSubclassOf<UUserWidget> WidgetClass = nullptr);
	
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|UI")
	UUserWidget* PopFromLayer(ECrowdyUILayer Layer, bool bRemoveFromParent = true);
	
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|UI")
	UOverlay* GetLayer(ECrowdyUILayer Layer);
};
