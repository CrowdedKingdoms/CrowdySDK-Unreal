#pragma once

#include "CoreMinimal.h"
#include "Core/UI/Enums/ECrowdyInputMode.h"
#include "Core/UI/Enums/ECrowdyUILayer.h"
#include "FWidgetSpec.generated.h"

USTRUCT(BlueprintType)
struct FWidgetSpec
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CrowdySDK|Widget")
	TSubclassOf<UUserWidget> WidgetClass;
	
	UPROPERTY(EditAnywhere,BlueprintReadOnly, Category="CrowdySDK|Widget")
	ECrowdyUILayer Layer = ECrowdyUILayer::GameLayer;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CrowdySDK|Widget")
	bool bClearLayerFirst = false;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CrowdySDK|Widget")
	bool bStartVisible = false;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CrowdySDK|Widget")
	ECrowdyInputMode EnteringInputMode = ECrowdyInputMode::GameAndUI;
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="CrowdySDK|Widget")
	ECrowdyInputMode ExitingInputMode = ECrowdyInputMode::Game;
};
