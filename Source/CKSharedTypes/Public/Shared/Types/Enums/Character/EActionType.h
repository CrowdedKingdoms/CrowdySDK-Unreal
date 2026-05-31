#pragma once

#include "CoreMinimal.h"
#include "EActionType.generated.h"

UENUM(BlueprintType)
enum class EActionType : uint8
{
	None UMETA(DisplayName = "None"),
	Hurdle UMETA(DisplayName = "Hurdle"),
	Vault UMETA(DisplayName = "Vault"),
	Mantle UMETA(DisplayName = "Mantle"),
};