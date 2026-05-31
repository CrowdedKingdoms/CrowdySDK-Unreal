#pragma once

#include "CoreMinimal.h"
#include "EGait.generated.h"

UENUM(BlueprintType)
enum class EGait : uint8
{
	Walk UMETA(DisplayName = "Walk"),
	Run UMETA(DisplayName = "Run"),
	Sprint UMETA(DisplayName = "Sprint"),
};
