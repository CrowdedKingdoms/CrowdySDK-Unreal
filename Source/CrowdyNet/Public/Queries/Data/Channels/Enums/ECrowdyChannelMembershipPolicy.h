#pragma once
#include "CoreMinimal.h"
#include "ECrowdyChannelMembershipPolicy.generated.h"

UENUM(BlueprintType)
enum class ECrowdyChannelMembershipPolicy : uint8
{
	Open    UMETA(DisplayName = "Open"),
	Request UMETA(DisplayName = "Request"),
	Invite  UMETA(DisplayName = "Invite"),
	Admin   UMETA(DisplayName = "Admin Only"),
};
