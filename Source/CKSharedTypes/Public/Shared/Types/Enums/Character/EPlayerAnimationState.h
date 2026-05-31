#pragma once

#include "CoreMinimal.h"
#include "EPlayerAnimationState.generated.h"

UENUM(BlueprintType)
enum class EPlayerAnimationState : uint8
{
	Standing UMETA(DisplayName = "Standing"),
	Crouched UMETA(DisplayName = "Crouched"),
	Jumping UMETA(DisplayName = "Jumping"),
	Falling UMETA(DisplayName = "Falling"),
	Landed UMETA(DisplayName = "Landed"),
	Flying UMETA(DisplayName = "Flying"),
};