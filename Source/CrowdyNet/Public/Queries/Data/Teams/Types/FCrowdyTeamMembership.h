#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Teams/Types/FCrowdyTeam.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamPermissions.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamRole.h"
#include "FCrowdyTeamMembership.generated.h"

/**
 * The signed-in player's own membership of one team: the team itself, the roles they hold in it, and the
 * permissions those roles add up to.
 */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyTeamMembership
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) FCrowdyTeam Team;

	/** Sorted by rank, lowest first. */
	UPROPERTY(BlueprintReadOnly) TArray<FCrowdyTeamRole> Roles;

	/** The effective permissions across every role held, as the server computed them. */
	UPROPERTY(BlueprintReadOnly) FCrowdyTeamPermissions Permissions;

	UPROPERTY(BlueprintReadOnly) FString JoinedAt;

	bool HasPermission(ECrowdyTeamPermission Permission) const
	{
		return Permissions.Has(Permission);
	}

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyTeamMembership& Out)
	{
		if (!Obj.IsValid()) return false;

		const TSharedPtr<FJsonObject>* TeamObj;
		if (Obj->TryGetObjectField(TEXT("group"), TeamObj))
			FCrowdyTeam::ParseFromJson(*TeamObj, Out.Team);

		const TArray<TSharedPtr<FJsonValue>>* RolesArray;
		if (Obj->TryGetArrayField(TEXT("roles"), RolesArray))
		{
			for (const TSharedPtr<FJsonValue>& Value : *RolesArray)
			{
				const TSharedPtr<FJsonObject>* RoleObj;
				if (Value->TryGetObject(RoleObj))
				{
					FCrowdyTeamRole Role;
					FCrowdyTeamRole::ParseFromJson(*RoleObj, Role);

					// A role nested in a membership is selected without its own team id, which is always this team's.
					if (Role.TeamId == 0) Role.TeamId = Out.Team.TeamId;

					Out.Roles.Add(Role);
				}
			}
			Out.Roles.Sort([](const FCrowdyTeamRole& A, const FCrowdyTeamRole& B) { return A.Rank < B.Rank; });
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
			Out.Permissions = FCrowdyTeamPermissions::FromStringArray(Keys);
		}

		Obj->TryGetStringField(TEXT("joinedAt"), Out.JoinedAt);
		return true;
	}
};
