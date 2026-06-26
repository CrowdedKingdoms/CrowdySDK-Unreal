#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Teams/Types/FCrowdyGroup.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupRole.h"
#include "Queries/Data/Teams/Types/FCrowdyRolePermissions.h"
#include "FCrowdyGroupMembership.generated.h"

USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyGroupMembership
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	FCrowdyGroup Group;

	UPROPERTY(BlueprintReadOnly)
	TArray<FCrowdyGroupRole> Roles;

	UPROPERTY(BlueprintReadOnly)
	FCrowdyRolePermissions Permissions;

	UPROPERTY(BlueprintReadOnly)
	FString JoinedAt;

	bool HasPermission(ECrowdyTeamPermission Permission) const
	{
		return Permissions.Has(Permission);
	}

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyGroupMembership& Out)
	{
		if (!Obj.IsValid()) return false;

		const TSharedPtr<FJsonObject>* GroupObj;
		if (Obj->TryGetObjectField(TEXT("group"), GroupObj))
			FCrowdyGroup::ParseFromJson(*GroupObj, Out.Group);

		const TArray<TSharedPtr<FJsonValue>>* RolesArr;
		if (Obj->TryGetArrayField(TEXT("roles"), RolesArr))
		{
			for (const TSharedPtr<FJsonValue>& V : *RolesArr)
			{
				const TSharedPtr<FJsonObject>* RoleObj;
				if (V->TryGetObject(RoleObj))
				{
					FCrowdyGroupRole Role;
					FCrowdyGroupRole::ParseFromJson(*RoleObj, Role);
					Out.Roles.Add(Role);
				}
			}
			Out.Roles.Sort([](const FCrowdyGroupRole& A, const FCrowdyGroupRole& B) { return A.Rank < B.Rank; });
		}

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

		Obj->TryGetStringField(TEXT("joinedAt"), Out.JoinedAt);
		return true;
	}
};
