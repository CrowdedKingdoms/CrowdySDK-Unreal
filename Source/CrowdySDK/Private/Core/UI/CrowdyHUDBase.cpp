// Fill out your copyright notice in the Description page of Project Settings.


#include "Core/UI/CrowdyHUDBase.h"
#include "Core/UI/Interfaces/CrowdyUIRootWidget.h"
#include "Blueprint/UserWidget.h"
#include "Components/Overlay.h"

void ACrowdyHUDBase::ExecuteUICommand_Implementation(FGameplayTag Command, bool bHideOtherWidgetsInLayer, bool bChangeInputMode)
{
	if (!WidgetSetConfig->WidgetSpecMap.Contains(Command))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Command not handled by this Widget Config."));
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
	
	CrowdySDK = Cast<UCrowdySDKSubsystem>(GetGameInstance()->GetSubsystem<UCrowdySDKSubsystem>());
	if (!IsValid(CrowdySDK))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Invalid Crowdy SDK subsystem."));
		return;
	}
	
	APlayerController* PlayerController = GetWorld()->GetFirstPlayerController();
	
	if (!IsValid(PlayerController))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Invalid player controller."));
		return;
	}
	
	CrowdyHUD = CreateWidget(PlayerController, HUDWidgetClass);
	
	CrowdyHUD->AddToViewport();
	
	if (!IsValid(CrowdyHUD))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Failed to create Crowdy HUD."));
		return;
	}
	
	UE_LOG(LogTemp, Log, TEXT("[CrowdySDK]: Created Crowdy HUD"));
	
	InitializeCrowdyWidgets();
}

void ACrowdyHUDBase::ToggleWidget(TSubclassOf<UUserWidget> WidgetClass, const ECrowdyUILayer Layer,
                                  const bool HideOtherWidgetsInLayer, const ECrowdyInputMode EnteringInputMode, const ECrowdyInputMode ExitingInputMode, const
                                  bool bChangeInputMode)
{
	
	UUserWidget* WidgetToToggle = Widgets.FindRef(WidgetClass);
	
	if (!IsValid(WidgetToToggle))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Invalid widget to toggle."));
		return;
	}

	const UOverlay* TargetPanel = ICrowdyUIRootWidget::Execute_GetLayer(CrowdyHUD, Layer);
	
	if (!IsValid(TargetPanel))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Invalid target panel for layer %d"), (int32)Layer);
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
	
	UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Widget not found for class %s. Check Widget Config Data Asset"), *WidgetClass->GetName());
	return nullptr;
}

void ACrowdyHUDBase::InitializeCrowdyWidgets_Implementation()
{
	if (!IsValid(WidgetSetConfig))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Invalid Widget Set Config."));
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
			UE_LOG(LogTemp, Error, TEXT("[CrowdySDK]: Failed to create widget for class %s."), *WidgetClass->GetName());
			continue;
		}
		
		CreatedWidget->SetVisibility(bStartVisible ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
		
		Widgets.Add(WidgetClass, CreatedWidget);
	}
	
	CrowdySDK->OnCrowdyHUDReady.Broadcast();
}

