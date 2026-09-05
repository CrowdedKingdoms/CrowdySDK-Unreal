#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Channels/Enums/ECrowdyChannelPermission.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelPermissions.h"
#include "FCrowdyChannelRole.generated.h"

/**
 * A named set of permissions within one channel. Every channel has a system "leader" role it cannot delete, and an
 * open chat channel also has a default "member" role granting send_messages to everyone who joins.
 */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyChannelRole
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64   ChannelRoleId = 0;
	UPROPERTY(BlueprintReadOnly) int64   ChannelId     = 0;
	UPROPERTY(BlueprintReadOnly) FString RoleName;
	UPROPERTY(BlueprintReadOnly) int32   Rank          = 0;
	UPROPERTY(BlueprintReadOnly) bool    bIsSystem     = false;
	UPROPERTY(BlueprintReadOnly) FCrowdyChannelPermissions Permissions;

	/**
	 * When the server returns this role nested inside a member it omits createdAt, so this is empty there and
	 * filled only when the role came from the channel's own role list.
	 */
	UPROPERTY(BlueprintReadOnly) FString CreatedAt;

	bool HasPermission(ECrowdyChannelPermission Permission) const
	{
		return Permissions.Has(Permission);
	}

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyChannelRole& Out)
	{
		if (!Obj.IsValid()) return false;

		FString RoleIdStr;
		if (Obj->TryGetStringField(TEXT("groupRoleId"), RoleIdStr)) Out.ChannelRoleId = FCString::Atoi64(*RoleIdStr);
		FString ChannelIdStr;
		if (Obj->TryGetStringField(TEXT("groupId"), ChannelIdStr))  Out.ChannelId     = FCString::Atoi64(*ChannelIdStr);

		Obj->TryGetStringField(TEXT("roleName"), Out.RoleName);
		Obj->TryGetNumberField(TEXT("rank"), Out.Rank);
		Obj->TryGetBoolField(TEXT("isSystem"), Out.bIsSystem);
		Obj->TryGetStringField(TEXT("createdAt"), Out.CreatedAt);

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

		return true;
	}
};
