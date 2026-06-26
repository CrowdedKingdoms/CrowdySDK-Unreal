#pragma once
#include "CoreMinimal.h"
#include "ECrowdyTeamCreationPolicy.generated.h"

UENUM(BlueprintType)
enum class ECrowdyTeamCreationPolicy : uint8
{
	Admin   UMETA(DisplayName = "Admin Only"),
	Member  UMETA(DisplayName = "Members"),
	Anyone  UMETA(DisplayName = "Anyone"),
};
