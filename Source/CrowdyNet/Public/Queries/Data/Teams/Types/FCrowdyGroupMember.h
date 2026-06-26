#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Teams/Types/FCrowdyGroupRole.h"
#include "FCrowdyGroupMember.generated.h"

USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyGroupMember
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64   GroupMemberId = 0;
	UPROPERTY(BlueprintReadOnly) int64   GroupId       = 0;
	UPROPERTY(BlueprintReadOnly) int64   UserId        = 0;
	UPROPERTY(BlueprintReadOnly) FString Status;
	UPROPERTY(BlueprintReadOnly) TArray<FCrowdyGroupRole> Roles;
	UPROPERTY(BlueprintReadOnly) FString CreatedAt;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyGroupMember& Out)
	{
		if (!Obj.IsValid()) return false;

		FString MidStr;
		if (Obj->TryGetStringField(TEXT("groupMemberId"), MidStr)) Out.GroupMemberId = FCString::Atoi64(*MidStr);
		FString GidStr;
		if (Obj->TryGetStringField(TEXT("groupId"), GidStr))       Out.GroupId       = FCString::Atoi64(*GidStr);
		FString UidStr;
		if (Obj->TryGetStringField(TEXT("userId"), UidStr))        Out.UserId        = FCString::Atoi64(*UidStr);

		Obj->TryGetStringField(TEXT("status"),    Out.Status);
		Obj->TryGetStringField(TEXT("createdAt"), Out.CreatedAt);

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
		return true;
	}
};
