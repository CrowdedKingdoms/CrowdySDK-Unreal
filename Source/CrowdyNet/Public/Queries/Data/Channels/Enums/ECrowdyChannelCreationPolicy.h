#pragma once
#include "CoreMinimal.h"
#include "ECrowdyChannelCreationPolicy.generated.h"

UENUM(BlueprintType)
enum class ECrowdyChannelCreationPolicy : uint8
{
	Admin   UMETA(DisplayName = "Admin Only"),
	Member  UMETA(DisplayName = "Members"),
	Anyone  UMETA(DisplayName = "Anyone"),
};
