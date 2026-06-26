#pragma once
#include "CoreMinimal.h"
#include "ECrowdyTeamPermission.generated.h"

UENUM(BlueprintType)
enum class ECrowdyTeamPermission : uint8
{
	ManageGroup   UMETA(DisplayName = "Manage Group"),
	ManageMembers UMETA(DisplayName = "Manage Members"),
	ManageRoles   UMETA(DisplayName = "Manage Roles"),
	InviteMembers UMETA(DisplayName = "Invite Members"),
};
