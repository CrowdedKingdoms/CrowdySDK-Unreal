#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamPermission.h"
#include "Queries/Data/Teams/Types/FCrowdyRolePermissions.h"
#include "FCrowdyGroupRole.generated.h"

USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyGroupRole
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int64 GroupRoleId = 0;

	UPROPERTY(BlueprintReadOnly)
	int64 GroupId = 0;

	UPROPERTY(BlueprintReadOnly)
	FString RoleName;

	UPROPERTY(BlueprintReadOnly)
	int32 Rank = 0;

	UPROPERTY(BlueprintReadOnly)
	bool bIsSystem = false;

	UPROPERTY(BlueprintReadOnly)
	FCrowdyRolePermissions Permissions;

	bool HasPermission(ECrowdyTeamPermission Permission) const
	{
		return Permissions.Has(Permission);
	}

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyGroupRole& Out)
	{
		if (!Obj.IsValid()) return false;

		FString IdStr;
		if (Obj->TryGetStringField(TEXT("groupRoleId"), IdStr)) Out.GroupRoleId = FCString::Atoi64(*IdStr);
		FString GidStr;
		if (Obj->TryGetStringField(TEXT("groupId"), GidStr))    Out.GroupId     = FCString::Atoi64(*GidStr);

		Obj->TryGetStringField(TEXT("roleName"), Out.RoleName);
		Obj->TryGetNumberField(TEXT("rank"), Out.Rank);
		Obj->TryGetBoolField(TEXT("isSystem"), Out.bIsSystem);

		const TArray<TSharedPtr<FJsonValue>>* PermsArr;
		if (Obj->TryGetArrayField(TEXT("permissions"), PermsArr))
		{
			TArray<FString> Raw;
			for (const TSharedPtr<FJsonValue>& V : *PermsArr)
			{
				FString P;
				if (V->TryGetString(P)) Raw.Add(P);
			}
			Out.Permissions = FCrowdyRolePermissions::FromStringArray(Raw);
		}
		return true;
	}

	static FString PermissionToString(ECrowdyTeamPermission P)
	{
		switch (P)
		{
		case ECrowdyTeamPermission::ManageGroup: return TEXT("manage_group");
		case ECrowdyTeamPermission::ManageMembers: return TEXT("manage_members");
		case ECrowdyTeamPermission::ManageRoles: return TEXT("manage_roles");
		case ECrowdyTeamPermission::InviteMembers: return TEXT("invite_members");
		}
		return TEXT("");
	}
};
