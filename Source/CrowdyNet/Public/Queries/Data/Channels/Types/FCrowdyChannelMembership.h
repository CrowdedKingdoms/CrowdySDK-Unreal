#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Channels/Types/FCrowdyChannel.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelPermissions.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelRole.h"
#include "FCrowdyChannelMembership.generated.h"

/**
 * The signed-in player's own membership of one channel: the channel itself, the roles they hold in it, and the
 * permissions those roles add up to. Check bSendMessages on Permissions to know whether they may post.
 */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyChannelMembership
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FCrowdyChannel Channel;

	/** Sorted by rank, lowest first. */
	UPROPERTY(BlueprintReadOnly) TArray<FCrowdyChannelRole> Roles;

	/** The effective permissions across every role held, as the server computed them. */
	UPROPERTY(BlueprintReadOnly) FCrowdyChannelPermissions Permissions;

	UPROPERTY(BlueprintReadOnly) FString JoinedAt;

	bool HasPermission(ECrowdyChannelPermission Permission) const
	{
		return Permissions.Has(Permission);
	}

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyChannelMembership& Out)
	{
		if (!Obj.IsValid()) return false;

		const TSharedPtr<FJsonObject>* ChannelObj;
		if (Obj->TryGetObjectField(TEXT("group"), ChannelObj))
			FCrowdyChannel::ParseFromJson(*ChannelObj, Out.Channel);

		const TArray<TSharedPtr<FJsonValue>>* RolesArray;
		if (Obj->TryGetArrayField(TEXT("roles"), RolesArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *RolesArray)
			{
				const TSharedPtr<FJsonObject>* RoleObj;
				if (Value->TryGetObject(RoleObj))
				{
					FCrowdyChannelRole Role;
					FCrowdyChannelRole::ParseFromJson(*RoleObj, Role);

					// A role nested in a membership is selected without its own channel id, which is always this one's.
					if (Role.ChannelId == 0) Role.ChannelId = Out.Channel.ChannelId;

					Out.Roles.Add(Role);
				}
			}
			Out.Roles.Sort([](const FCrowdyChannelRole& A, const FCrowdyChannelRole& B) { return A.Rank < B.Rank; });
		}

		const TArray<TSharedPtr<FJsonValue>>* PermissionsArray;
		if (Obj->TryGetArrayField(TEXT("permissions"), PermissionsArray))
		{
			TArray<FString> Keys;
			for (const TSharedPtr<FJsonValue>& Value : *PermissionsArray)
			{
				FString Key;
				if (Value->TryGetString(Key)) Keys.Add(Key);
			}
			Out.Permissions = FCrowdyChannelPermissions::FromStringArray(Keys);
		}

		Obj->TryGetStringField(TEXT("joinedAt"), Out.JoinedAt);
		return true;
	}
};
