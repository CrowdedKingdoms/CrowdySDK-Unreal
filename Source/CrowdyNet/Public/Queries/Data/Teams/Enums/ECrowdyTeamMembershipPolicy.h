#pragma once
#include "CoreMinimal.h"
#include "ECrowdyTeamMembershipPolicy.generated.h"

UENUM(BlueprintType)
enum class ECrowdyTeamMembershipPolicy : uint8
{
	Open    UMETA(DisplayName = "Open"),
	Request UMETA(DisplayName = "Request"),
	Invite  UMETA(DisplayName = "Invite"),
	Admin   UMETA(DisplayName = "Admin Only"),
};
