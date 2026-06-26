// Fill out your copyright notice in the Description page of Project Settings.


#include "Core/UI/CrowdyHUDBase.h"
#include "CrowdyServicesLog.h"
#include "Core/UI/Interfaces/CrowdyUIRootWidget.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Components/Overlay.h"

void ACrowdyHUDBase::ExecuteUICommand_Implementation(FGameplayTag Command, bool bHideOtherWidgetsInLayer, bool bChangeInputMode)
{
	if (!WidgetSetConfig->WidgetSpecMap.Contains(Command))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("Command not handled by this Widget Config."));
		return;
	}

	const TSubclassOf<UUserWidget> WidgetClass = WidgetSetConfig->WidgetSpecMap[Command].WidgetClass;
	const ECrowdyUILayer Layer = WidgetSetConfig->WidgetSpecMap[Command].Layer;
	const ECrowdyInputMode EnteringInputMode = WidgetSetConfig->WidgetSpecMap[Command].EnteringInputMode;
	const ECrowdyInputMode ExitingInputMode = WidgetSetConfig->WidgetSpecMap[Command].ExitingInputMode;
	
	ToggleWidget(WidgetClass, Layer, bHideOtherWidgetsInLayer, EnteringInputMode, ExitingInputMode, bChangeInputMode);
}

UUserWidget* ACrowdyHUDBase::GetWidgetReference_Implementation(TSubclassOf<UUserWidget> WidgetClass)
{
	return GetWidget(WidgetClass);
}

void ACrowdyHUDBase::ToggleLayerVisibility_Implementation(ECrowdyUILayer Layer, bool bVisible)
{
	UOverlay* TargetPanel = ICrowdyUIRootWidget::Execute_GetLayer(CrowdyHUD, Layer);
	TargetPanel->SetVisibility(bVisible ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
}

void ACrowdyHUDBase::BeginPlay()
{
	Super::BeginPlay();
	
	UCrowdySDKBridgeSubsystem* SDKBridge = GetGameInstance()->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	if (!IsValid(SDKBridge) || !SDKBridge->ServiceRegistry)
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("Crowdy SDK bridge not ready."));
		return;
	}
	
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	
	if (!IsValid(PlayerController))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("Invalid player controller."));
		return;
	}
	
	CrowdyHUD = CreateWidget(PlayerController, HUDWidgetClass);
	
	CrowdyHUD->AddToViewport();
	
	if (!IsValid(CrowdyHUD))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("Failed to create Crowdy HUD."));
		return;
	}
	
	UE_CLOG(CrowdyServicesTrace::Hud(), LogCrowdyServices, Log, TEXT("Created Crowdy HUD"));
	
	InitializeCrowdyWidgets();
}

void ACrowdyHUDBase::ToggleWidget(TSubclassOf<UUserWidget> WidgetClass, const ECrowdyUILayer Layer,
                                  const bool HideOtherWidgetsInLayer, const ECrowdyInputMode EnteringInputMode, const ECrowdyInputMode ExitingInputMode, const
                                  bool bChangeInputMode)
{
	
	UUserWidget* WidgetToToggle = Widgets.FindRef(WidgetClass);
	
	if (!IsValid(WidgetToToggle))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("Invalid widget to toggle."));
		return;
	}

	const UOverlay* TargetPanel = ICrowdyUIRootWidget::Execute_GetLayer(CrowdyHUD, Layer);
	
	if (!IsValid(TargetPanel))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("Invalid target panel for layer %d"), (int32)Layer);
		return;
	}
	
	if (WidgetToToggle->IsVisible())
	{
		WidgetToToggle->SetVisibility(ESlateVisibility::Collapsed);
		
		if (HideOtherWidgetsInLayer)
		{
			for (const auto& Child : TargetPanel->GetAllChildren())
			{
				Child->SetVisibility(ESlateVisibility::Collapsed);
			}
		}
		
		APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
		
		if (!bChangeInputMode)
		{
			return;
		}
		
		switch (ExitingInputMode)
		{
		case ECrowdyInputMode::Game:
			PlayerController->bShowMouseCursor = false;
			PlayerController->SetInputMode(FInputModeGameOnly());
			break;
		case ECrowdyInputMode::UI:
			PlayerController->SetInputMode(FInputModeUIOnly());
			PlayerController->bShowMouseCursor = true;
			break;
		case ECrowdyInputMode::GameAndUI:
			PlayerController->SetInputMode(FInputModeGameAndUI());
			PlayerController->bShowMouseCursor = true;
			break;
		}
		
		return;
	}
	
	if (HideOtherWidgetsInLayer)
	{
		for (const auto& Child : TargetPanel->GetAllChildren())
		{
			Child->SetVisibility(ESlateVisibility::Collapsed);
		}
	}
	
	WidgetToToggle->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	
	if (WidgetToToggle->IsFocusable())
	{
		WidgetToToggle->SetFocus();
	}

	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();

	if (!bChangeInputMode)
	{
		return;
	}
	
	switch (EnteringInputMode)
	{
	case ECrowdyInputMode::Game:
		PlayerController->bShowMouseCursor = false;
		PlayerController->SetInputMode(FInputModeGameOnly());
		break;
	case ECrowdyInputMode::UI:
		PlayerController->SetInputMode(FInputModeUIOnly());
		PlayerController->bShowMouseCursor = true;
		break;
	case ECrowdyInputMode::GameAndUI:
		PlayerController->SetInputMode(FInputModeGameAndUI());
		PlayerController->bShowMouseCursor = true;
		break;
	}
}

UUserWidget* ACrowdyHUDBase::GetWidget(const TSubclassOf<UUserWidget> WidgetClass)
{
	for (const auto& WidgetPair : Widgets)
	{
		if (WidgetPair.Key == WidgetClass)
		{
			return WidgetPair.Value;
		}
	}
	
	UE_LOG(LogCrowdyServices, Error, TEXT("Widget not found for class %s. Check Widget Config Data Asset"), *WidgetClass->GetName());
	return nullptr;
}

void ACrowdyHUDBase::InitializeCrowdyWidgets_Implementation()
{
	if (!IsValid(WidgetSetConfig))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("Invalid Widget Set Config."));
		return;
	}
	
	for (const auto& WidgetSpec : WidgetSetConfig->WidgetSpecMap)
	{
		auto& WidgetSpecValue = WidgetSpec.Value;
		const ECrowdyUILayer Layer = WidgetSpecValue.Layer;
		const bool bClearLayerFirst = WidgetSpecValue.bClearLayerFirst;
		const bool bStartVisible = WidgetSpecValue.bStartVisible;
		const TSubclassOf<UUserWidget> WidgetClass = WidgetSpecValue.WidgetClass;
		
		UUserWidget* CreatedWidget = ICrowdyUIRootWidget::Execute_PushToLayer(CrowdyHUD, Layer, bClearLayerFirst, WidgetClass);
		
		if (!IsValid(CreatedWidget))
		{
			UE_LOG(LogCrowdyServices, Error, TEXT("Failed to create widget for class %s."), *WidgetClass->GetName());
			continue;
		}
		
		CreatedWidget->SetVisibility(bStartVisible ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
		
		Widgets.Add(WidgetClass, CreatedWidget);
	}
	
	if (UCrowdySDKBridgeSubsystem* Bridge = GetGameInstance()->GetSubsystem<UCrowdySDKBridgeSubsystem>())
	{
		if (Bridge->BroadcastHUDReadyFn)
			Bridge->BroadcastHUDReadyFn();
	}
}

