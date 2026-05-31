#pragma once

#include "CoreMinimal.h"
#include "ECrowdyUILayer.generated.h"

UENUM(BlueprintType)
enum class ECrowdyUILayer : uint8
{
	GameLayer UMETA(DisplayName="Game Layer"),
	GameMenuLayer UMETA(DisplayName="Game Menu Layer"),
	MenuLayer UMETA(DisplayName="Menu Layer"),
	ModalLayer UMETA(DisplayName="Modal Layer"),
};
