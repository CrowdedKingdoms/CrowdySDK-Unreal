#pragma once
#include "CoreMinimal.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamPermission.h"
#include "FCrowdyRolePermissions.generated.h"

USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyRolePermissions
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Teams") bool bManageGroup   = false;
	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Teams") bool bManageMembers = false;
	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Teams") bool bManageRoles   = false;
	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Teams") bool bInviteMembers = false;

	bool Has(ECrowdyTeamPermission P) const
	{
		switch (P)
		{
		case ECrowdyTeamPermission::ManageGroup:   return bManageGroup;
		case ECrowdyTeamPermission::ManageMembers: return bManageMembers;
		case ECrowdyTeamPermission::ManageRoles:   return bManageRoles;
		case ECrowdyTeamPermission::InviteMembers: return bInviteMembers;
		}
		return false;
	}

	TArray<FString> ToStringArray() const
	{
		TArray<FString> Out;
		if (bManageGroup)   Out.Add(TEXT("manage_group"));
		if (bManageMembers) Out.Add(TEXT("manage_members"));
		if (bManageRoles)   Out.Add(TEXT("manage_roles"));
		if (bInviteMembers) Out.Add(TEXT("invite_members"));
		return Out;
	}

	static FCrowdyRolePermissions FromStringArray(const TArray<FString>& Perms)
	{
		FCrowdyRolePermissions Out;
		Out.bManageGroup   = Perms.Contains(TEXT("manage_group"));
		Out.bManageMembers = Perms.Contains(TEXT("manage_members"));
		Out.bManageRoles   = Perms.Contains(TEXT("manage_roles"));
		Out.bInviteMembers = Perms.Contains(TEXT("invite_members"));
		return Out;
	}
};
