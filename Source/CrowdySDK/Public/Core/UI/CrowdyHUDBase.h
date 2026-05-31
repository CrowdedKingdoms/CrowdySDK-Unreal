// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyWidgetSet.h"
#include "Enums/ECrowdyUILayer.h"
#include "GameFramework/HUD.h"
#include "Interfaces/CrowdyHUD.h"
#include "Interfaces/CrowdyUIRootWidget.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "CrowdyHUDBase.generated.h"

/**
 * 
 */
UCLASS(Blueprintable, BlueprintType, Abstract)
class CROWDYSDK_API ACrowdyHUDBase : public AHUD, public ICrowdyHUD
{
	GENERATED_BODY()

public:
	
	virtual void ExecuteUICommand_Implementation(FGameplayTag Command, bool bHideOtherWidgetsInLayer = false, bool bChangeInputMode = true) override;
	virtual UUserWidget* GetWidgetReference_Implementation(TSubclassOf<UUserWidget> WidgetClass) override;
	virtual void ToggleLayerVisibility_Implementation(ECrowdyUILayer Layer, bool bVisible = false) override;
	
protected:
	
	virtual void BeginPlay() override;
	
	UPROPERTY(Transient, EditAnywhere, BlueprintReadWrite, Category = "CrowdySDK|HUD|Widgets", meta =(DisplayName="HUD Widget Class"))
	TSubclassOf<UUserWidget> HUDWidgetClass = nullptr;
	
	UPROPERTY(Transient, EditAnywhere, BlueprintReadWrite, Category = "CrowdySDK|HUD|Widgets", meta =(DisplayName="Widget Set Config"))
	UCrowdyWidgetSet* WidgetSetConfig = nullptr;
	
	UFUNCTION(BlueprintCallable, BlueprintNativeEvent, Category = "CrowdySDK|HUD")
	void InitializeCrowdyWidgets();

private:
	
	UPROPERTY()
	TMap<TSubclassOf<UUserWidget>, TObjectPtr<UUserWidget>> Widgets;
	
	UPROPERTY(Transient)
	UUserWidget* CrowdyHUD = nullptr;
	
	UPROPERTY()
	UCrowdySDKSubsystem* CrowdySDK;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|HUD")
	void ToggleWidget(TSubclassOf<UUserWidget> WidgetClass, const ECrowdyUILayer Layer, const bool HideOtherWidgetsInLayer = true, ECrowdyInputMode
	                  EnteringInputMode = ECrowdyInputMode::Game, ECrowdyInputMode ExitingInputMode = ECrowdyInputMode::Game, bool
	                  bChangeInputMode = true);
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|HUD")
	UUserWidget* GetWidget(TSubclassOf<UUserWidget> WidgetClass);
};
