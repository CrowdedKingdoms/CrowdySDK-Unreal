// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/Interface.h"
#include "CrowdyHUD.generated.h"

// This class does not need to be modified.
UINTERFACE(BlueprintType)
class UCrowdyHUD : public UInterface
{
	GENERATED_BODY()
};

/**
 * 
 */
class CROWDYSDK_API ICrowdyHUD
{
	GENERATED_BODY()

	// Add interface functions to this class. This is the class that will be inherited to implement this interface.
public:
	
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|HUD", meta=(DisplayName="Execute Crowdy UI Command"))
	void ExecuteUICommand(FGameplayTag Command, bool bHideOtherWidgetsInLayer = false, bool bChangeInputMode = true);
	
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|HUD", meta=(DisplayName="Get Widget Reference"))
	UUserWidget* GetWidgetReference(TSubclassOf<UUserWidget> WidgetClass);
	
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|HUD", meta=(DisplayName="Toggle Layer Visibility"))
	void ToggleLayerVisibility(ECrowdyUILayer Layer, bool bVisible = false);
};
