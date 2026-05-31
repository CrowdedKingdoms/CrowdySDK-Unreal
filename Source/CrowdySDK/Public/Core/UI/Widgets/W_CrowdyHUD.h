// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Overlay.h"
#include "Core/UI/Interfaces/CrowdyUIRootWidget.h"
#include "W_CrowdyHUD.generated.h"

/**
 * 
 */
UCLASS(Blueprintable, BlueprintType)
class CROWDYSDK_API UW_CrowdyHUD : public UUserWidget, public ICrowdyUIRootWidget
{
	GENERATED_BODY()
	
public:
	virtual void NativeConstruct() override;
	
	virtual UUserWidget* PushToLayer_Implementation(ECrowdyUILayer Layer, bool bClearLayerFirst = false, TSubclassOf<UUserWidget> WidgetClass = nullptr) override;
	virtual UUserWidget* PopFromLayer_Implementation(ECrowdyUILayer Layer, bool bRemoveFromParent = true) override;
	virtual UOverlay* GetLayer_Implementation(ECrowdyUILayer Layer) override;
	
protected:
	
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UOverlay> GameLayer;
	
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UOverlay> GameMenuLayer;
	
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UOverlay> MenuLayer;
	
	UPROPERTY(meta = (BindWidget))
	TObjectPtr<UOverlay> ModalLayer;
	
};
