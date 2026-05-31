#pragma once
#include "CoreMinimal.h"

UENUM(BlueprintType)
enum class EPlayerCameraView : uint8
{
	ThirdPerson = 0 UMETA(DisplayName = "Third Person"),
	FirstPerson = 1 UMETA(DisplayName = "First Person"),
};