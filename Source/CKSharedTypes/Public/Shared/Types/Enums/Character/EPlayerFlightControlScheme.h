#pragma once

#include "CoreMinimal.h"

UENUM(BlueprintType)
enum class EPlayerFlightControlScheme : uint8
{
	Advanced = 0 UMETA(DisplayName = "Advanced"),
	Simple = 1 UMETA(DisplayName = "Simple"),
};