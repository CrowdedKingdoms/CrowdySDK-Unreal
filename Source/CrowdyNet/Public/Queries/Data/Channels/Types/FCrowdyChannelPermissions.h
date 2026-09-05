#pragma once
#include "CoreMinimal.h"
#include "Queries/Data/Channels/Enums/ECrowdyChannelPermission.h"
#include "FCrowdyChannelPermissions.generated.h"

/**
 * The permissions a channel role grants, as flags rather than raw key strings.
 *
 * Channels do not share this type with teams because they do not share a permission set: send_messages decides who
 * may publish to a channel and has no team equivalent.
 */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyChannelPermissions
{
	GENERATED_BODY()

	/** Publish to the channel. Receiving is a property of membership and needs no permission. */
	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Channels") bool bSendMessages  = false;

	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Channels") bool bManageChannel = false;
	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Channels") bool bManageMembers = false;
	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Channels") bool bManageRoles   = false;
	UPROPERTY(BlueprintReadWrite, Category="Crowdy|Channels") bool bInviteMembers = false;

	bool Has(ECrowdyChannelPermission Permission) const
	{
		switch (Permission)
		{
		case ECrowdyChannelPermission::SendMessages:  return bSendMessages;
		case ECrowdyChannelPermission::ManageChannel: return bManageChannel;
		case ECrowdyChannelPermission::ManageMembers: return bManageMembers;
		case ECrowdyChannelPermission::ManageRoles:   return bManageRoles;
		case ECrowdyChannelPermission::InviteMembers: return bInviteMembers;
		}
		return false;
	}

	TArray<FString> ToStringArray() const
	{
		TArray<FString> Out;
		if (bSendMessages)  Out.Add(TEXT("send_messages"));
		if (bManageChannel) Out.Add(TEXT("manage_group"));
		if (bManageMembers) Out.Add(TEXT("manage_members"));
		if (bManageRoles)   Out.Add(TEXT("manage_roles"));
		if (bInviteMembers) Out.Add(TEXT("invite_members"));
		return Out;
	}

	static FCrowdyChannelPermissions FromStringArray(const TArray<FString>& Permissions)
	{
		FCrowdyChannelPermissions Out;
		Out.bSendMessages  = Permissions.Contains(TEXT("send_messages"));
		Out.bManageChannel = Permissions.Contains(TEXT("manage_group"));
		Out.bManageMembers = Permissions.Contains(TEXT("manage_members"));
		Out.bManageRoles   = Permissions.Contains(TEXT("manage_roles"));
		Out.bInviteMembers = Permissions.Contains(TEXT("invite_members"));
		return Out;
	}

	// The server's key for a permission. The channel-scoped "manage everything" key is spelled manage_group because
	// teams and channels share one permission table on the server.
	static FString ToKey(ECrowdyChannelPermission Permission)
	{
		switch (Permission)
		{
		case ECrowdyChannelPermission::SendMessages:  return TEXT("send_messages");
		case ECrowdyChannelPermission::ManageChannel: return TEXT("manage_group");
		case ECrowdyChannelPermission::ManageMembers: return TEXT("manage_members");
		case ECrowdyChannelPermission::ManageRoles:   return TEXT("manage_roles");
		case ECrowdyChannelPermission::InviteMembers: return TEXT("invite_members");
		}
		return FString();
	}
};
