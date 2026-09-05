#pragma once
#include "CoreMinimal.h"
#include "ECrowdyTeamPermission.generated.h"

UENUM(BlueprintType)
enum class ECrowdyTeamPermission : uint8
{
	ManageTeam    UMETA(DisplayName = "Manage Team"),
	ManageMembers UMETA(DisplayName = "Manage Members"),
	ManageRoles   UMETA(DisplayName = "Manage Roles"),
	InviteMembers UMETA(DisplayName = "Invite Members"),
};
