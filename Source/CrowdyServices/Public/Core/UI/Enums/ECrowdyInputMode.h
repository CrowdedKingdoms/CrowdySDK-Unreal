#pragma once

#include "CoreMinimal.h"
#include "ECrowdyInputMode.generated.h"

UENUM(BlueprintType)
enum class ECrowdyInputMode : uint8
{
	Game UMETA(DisplayName="Game Mode"),
	GameAndUI UMETA(DisplayName="Game and UI Mode"),
	UI UMETA(DisplayName="UI Mode"),
	
};
