// Fill out your copyright notice in the Description page of Project Settings.


#include "Core/UI/Widgets/W_CrowdyHUD.h"
#include "Components/OverlaySlot.h"

void UW_CrowdyHUD::NativeConstruct()
{
	Super::NativeConstruct();
}

UUserWidget* UW_CrowdyHUD::PushToLayer_Implementation(const ECrowdyUILayer Layer, const bool bClearLayerFirst,
                                                       TSubclassOf<UUserWidget> WidgetClass)
{
	if (!WidgetClass)
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Widget class is null.")); 
		return nullptr;
	}
	
	UOverlay* TargetPanel = Execute_GetLayer(this, Layer);
	
	if (!IsValid(TargetPanel))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Invalid target panel for layer %d"), (int32)Layer);
		return nullptr;
	}
	
	if (bClearLayerFirst) TargetPanel->ClearChildren();
	
	APlayerController* PlayerController = GetOwningPlayer();
	
	if (!IsValid(PlayerController))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Invalid player controller."));
		return nullptr;
	}
	
	UUserWidget* CreatedWidget = CreateWidget<UUserWidget>(PlayerController, WidgetClass);
	
	UOverlaySlot* PanelSlot = Cast<UOverlaySlot>(TargetPanel->AddChild(CreatedWidget));
	
	if (!IsValid(PanelSlot))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Failed to add child to target panel."));
		CreatedWidget->RemoveFromParent();
		return nullptr;
	}
	
	PanelSlot->SetVerticalAlignment(EVerticalAlignment::VAlign_Fill);
	PanelSlot->SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
	
	return CreatedWidget;
	
}

UUserWidget* UW_CrowdyHUD::PopFromLayer_Implementation(ECrowdyUILayer Layer, bool bRemoveFromParent)
{
	UOverlay* TargetPanel = Execute_GetLayer(this, Layer);
	if (!IsValid(TargetPanel))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Invalid target panel for layer %d"), (int32)Layer);
		return nullptr;
	}
	
	const int32 NumChildren = TargetPanel->GetChildrenCount();
	
	if (NumChildren <= 0) return nullptr;
	
	UWidget* TopWidget = TargetPanel->GetChildAt(NumChildren - 1);
	UUserWidget* TopUserWidget = Cast<UUserWidget>(TopWidget);
	
	if (!IsValid(TopUserWidget))
	{
		if (bRemoveFromParent && TopWidget)
		{
			TopWidget->RemoveFromParent();
		}
		return nullptr;
	}
	
	if (bRemoveFromParent) TopWidget->RemoveFromParent();
	
	return TopUserWidget;
}

UOverlay* UW_CrowdyHUD::GetLayer_Implementation(const ECrowdyUILayer Layer)
{
	switch (Layer)
	{
	case ECrowdyUILayer::GameLayer:
		return GameLayer;
	case ECrowdyUILayer::GameMenuLayer:
		return GameMenuLayer;
	case ECrowdyUILayer::MenuLayer:
		return MenuLayer;
	case ECrowdyUILayer::ModalLayer:
		return ModalLayer;
	default:
		return nullptr;
	}
}
