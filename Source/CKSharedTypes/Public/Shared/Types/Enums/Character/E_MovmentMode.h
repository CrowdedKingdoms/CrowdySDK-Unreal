#pragma once

#include "CoreMinimal.h"
#include "E_MovmentMode.generated.h"

UENUM(BlueprintType)
enum class E_MovementMode : uint8
{
	Parkour UMETA(DisplayName = "Parkour"),
	Traditional UMETA(DisplayName = "Traditional"),
};