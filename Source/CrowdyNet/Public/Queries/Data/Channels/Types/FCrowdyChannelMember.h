#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Channels/Types/FCrowdyChannelRole.h"
#include "FCrowdyChannelMember.generated.h"

/** One user's membership row in a channel, including the roles they hold there. */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyChannelMember
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64   ChannelMemberId = 0;
	UPROPERTY(BlueprintReadOnly) int64   ChannelId       = 0;
	UPROPERTY(BlueprintReadOnly) int64   UserId          = 0;

	/** "active" for a full member, "pending" while a join request is awaiting a decision. */
	UPROPERTY(BlueprintReadOnly) FString Status;

	/** Sorted by rank, lowest first. */
	UPROPERTY(BlueprintReadOnly) TArray<FCrowdyChannelRole> Roles;

	UPROPERTY(BlueprintReadOnly) FString CreatedAt;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyChannelMember& Out)
	{
		if (!Obj.IsValid()) return false;

		FString MemberIdStr;
		if (Obj->TryGetStringField(TEXT("groupMemberId"), MemberIdStr)) Out.ChannelMemberId = FCString::Atoi64(*MemberIdStr);
		FString ChannelIdStr;
		if (Obj->TryGetStringField(TEXT("groupId"), ChannelIdStr))      Out.ChannelId       = FCString::Atoi64(*ChannelIdStr);
		FString UserIdStr;
		if (Obj->TryGetStringField(TEXT("userId"), UserIdStr))          Out.UserId          = FCString::Atoi64(*UserIdStr);

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
					FCrowdyChannelRole Role;
					FCrowdyChannelRole::ParseFromJson(*RoleObj, Role);

					// A role nested in a member is selected without its own channel id, which is always this member's.
					if (Role.ChannelId == 0) Role.ChannelId = Out.ChannelId;

					Out.Roles.Add(Role);
				}
			}
			Out.Roles.Sort([](const FCrowdyChannelRole& A, const FCrowdyChannelRole& B) { return A.Rank < B.Rank; });
		}

		return true;
	}
};
