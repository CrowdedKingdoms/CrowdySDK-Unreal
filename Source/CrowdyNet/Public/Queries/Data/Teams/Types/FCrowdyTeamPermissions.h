#pragma once
#include "CoreMinimal.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamPermission.h"
#include "FCrowdyTeamPermissions.generated.h"

/**
 * The permissions a team role grants, as flags rather than raw key strings.
 *
 * Teams and channels do not share this type because they do not share a permission set: a channel additionally has
 * send_messages, which decides who may publish, and a team has no equivalent.
 */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyTeamPermissions
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Teams") bool bManageTeam    = false;
	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Teams") bool bManageMembers = false;
	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Teams") bool bManageRoles   = false;
	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Teams") bool bInviteMembers = false;

	bool Has(ECrowdyTeamPermission Permission) const
	{
		switch (Permission)
		{
		case ECrowdyTeamPermission::ManageTeam:    return bManageTeam;
		case ECrowdyTeamPermission::ManageMembers: return bManageMembers;
		case ECrowdyTeamPermission::ManageRoles:   return bManageRoles;
		case ECrowdyTeamPermission::InviteMembers: return bInviteMembers;
		}
		return false;
	}

	TArray<FString> ToStringArray() const
	{
		TArray<FString> Out;
		if (bManageTeam)    Out.Add(TEXT("manage_group"));
		if (bManageMembers) Out.Add(TEXT("manage_members"));
		if (bManageRoles)   Out.Add(TEXT("manage_roles"));
		if (bInviteMembers) Out.Add(TEXT("invite_members"));
		return Out;
	}

	static FCrowdyTeamPermissions FromStringArray(const TArray<FString>& Permissions)
	{
		FCrowdyTeamPermissions Out;
		Out.bManageTeam    = Permissions.Contains(TEXT("manage_group"));
		Out.bManageMembers = Permissions.Contains(TEXT("manage_members"));
		Out.bManageRoles   = Permissions.Contains(TEXT("manage_roles"));
		Out.bInviteMembers = Permissions.Contains(TEXT("invite_members"));
		return Out;
	}

	// The server's key for a permission. The team-scoped "manage everything" key is spelled manage_group because
	// teams and channels share one permission table on the server.
	static FString ToKey(ECrowdyTeamPermission Permission)
	{
		switch (Permission)
		{
		case ECrowdyTeamPermission::ManageTeam:    return TEXT("manage_group");
		case ECrowdyTeamPermission::ManageMembers: return TEXT("manage_members");
		case ECrowdyTeamPermission::ManageRoles:   return TEXT("manage_roles");
		case ECrowdyTeamPermission::InviteMembers: return TEXT("invite_members");
		}
		return FString();
	}
};
