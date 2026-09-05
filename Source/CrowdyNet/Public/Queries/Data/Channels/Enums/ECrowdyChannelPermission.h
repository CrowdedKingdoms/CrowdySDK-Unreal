#pragma once
#include "CoreMinimal.h"
#include "ECrowdyChannelPermission.generated.h"

UENUM(BlueprintType)
enum class ECrowdyChannelPermission : uint8
{
	SendMessages   UMETA(DisplayName = "Send Messages"),
	ManageChannel  UMETA(DisplayName = "Manage Channel"),
	ManageMembers  UMETA(DisplayName = "Manage Members"),
	ManageRoles    UMETA(DisplayName = "Manage Roles"),
	InviteMembers  UMETA(DisplayName = "Invite Members"),
};
