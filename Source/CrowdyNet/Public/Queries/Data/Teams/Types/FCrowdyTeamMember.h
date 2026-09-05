#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Teams/Types/FCrowdyTeamRole.h"
#include "FCrowdyTeamMember.generated.h"

/** One user's membership row in a team, including the roles they hold there. */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyTeamMember
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64   TeamMemberId = 0;
	UPROPERTY(BlueprintReadOnly) int64   TeamId       = 0;
	UPROPERTY(BlueprintReadOnly) int64   UserId       = 0;

	/** "active" for a full member, "pending" while a join request is awaiting a decision. */
	UPROPERTY(BlueprintReadOnly) FString Status;

	/** Sorted by rank, lowest first. */
	UPROPERTY(BlueprintReadOnly) TArray<FCrowdyTeamRole> Roles;

	UPROPERTY(BlueprintReadOnly) FString CreatedAt;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyTeamMember& Out)
	{
		if (!Obj.IsValid()) return false;

		FString MemberIdStr;
		if (Obj->TryGetStringField(TEXT("groupMemberId"), MemberIdStr)) Out.TeamMemberId = FCString::Atoi64(*MemberIdStr);
		FString TeamIdStr;
		if (Obj->TryGetStringField(TEXT("groupId"), TeamIdStr))         Out.TeamId       = FCString::Atoi64(*TeamIdStr);
		FString UserIdStr;
		if (Obj->TryGetStringField(TEXT("userId"), UserIdStr))          Out.UserId       = FCString::Atoi64(*UserIdStr);

		Obj->TryGetStringField(TEXT("status"),    Out.Status);
		Obj->TryGetStringField(TEXT("createdAt"), Out.CreatedAt);

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

					// A role nested in a member is selected without its own team id, which is always this member's.
					if (Role.TeamId == 0) Role.TeamId = Out.TeamId;

					Out.Roles.Add(Role);
				}
			}
			Out.Roles.Sort([](const FCrowdyTeamRole& A, const FCrowdyTeamRole& B) { return A.Rank < B.Rank; });
		}

		return true;
	}
};
