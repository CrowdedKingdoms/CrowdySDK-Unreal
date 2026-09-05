#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamPermission.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamPermissions.h"
#include "FCrowdyTeamRole.generated.h"

/** A named set of permissions within one team. Every team has a system "leader" role it cannot delete. */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyTeamRole
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64   TeamRoleId = 0;
	UPROPERTY(BlueprintReadOnly) int64   TeamId     = 0;
	UPROPERTY(BlueprintReadOnly) FString RoleName;
	UPROPERTY(BlueprintReadOnly) int32   Rank       = 0;
	UPROPERTY(BlueprintReadOnly) bool    bIsSystem  = false;
	UPROPERTY(BlueprintReadOnly) FCrowdyTeamPermissions Permissions;

	/**
	 * When the server returns this role nested inside a member it omits createdAt, so this is empty there and
	 * filled only when the role came from the team's own role list.
	 */
	UPROPERTY(BlueprintReadOnly) FString CreatedAt;

	bool HasPermission(ECrowdyTeamPermission Permission) const
	{
		return Permissions.Has(Permission);
	}

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyTeamRole& Out)
	{
		if (!Obj.IsValid()) return false;

		FString RoleIdStr;
		if (Obj->TryGetStringField(TEXT("groupRoleId"), RoleIdStr)) Out.TeamRoleId = FCString::Atoi64(*RoleIdStr);
		FString TeamIdStr;
		if (Obj->TryGetStringField(TEXT("groupId"), TeamIdStr))     Out.TeamId     = FCString::Atoi64(*TeamIdStr);

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
			Out.Permissions = FCrowdyTeamPermissions::FromStringArray(Keys);
		}

		return true;
	}
};
