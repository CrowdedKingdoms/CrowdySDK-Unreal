#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamCreationPolicy.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamMembershipPolicy.h"
#include "FCrowdyAppGroupPolicy.generated.h"

USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyAppGroupPolicy
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int64 AppId = 0;

	UPROPERTY(BlueprintReadOnly)
	FString GroupType;

	UPROPERTY(BlueprintReadOnly)
	ECrowdyTeamCreationPolicy CreationPolicy = ECrowdyTeamCreationPolicy::Anyone;

	UPROPERTY(BlueprintReadOnly)
	ECrowdyTeamMembershipPolicy DefaultMembershipPolicy = ECrowdyTeamMembershipPolicy::Open;

	UPROPERTY(BlueprintReadOnly)
	int32 MaxMembers = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 MaxGroupsPerUser = 0;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyAppGroupPolicy& Out)
	{
		if (!Obj.IsValid()) return false;

		FString AidStr;
		if (Obj->TryGetStringField(TEXT("appId"), AidStr)) Out.AppId = FCString::Atoi64(*AidStr);

		Obj->TryGetStringField(TEXT("groupType"), Out.GroupType);
		Obj->TryGetNumberField(TEXT("maxMembers"), Out.MaxMembers);
		Obj->TryGetNumberField(TEXT("maxGroupsPerUser"), Out.MaxGroupsPerUser);

		FString CreateStr;
		if (Obj->TryGetStringField(TEXT("creationPolicy"), CreateStr))
		{
			if (CreateStr == TEXT("admin")) Out.CreationPolicy = ECrowdyTeamCreationPolicy::Admin;
			else if (CreateStr == TEXT("member")) Out.CreationPolicy = ECrowdyTeamCreationPolicy::Member;
			else Out.CreationPolicy = ECrowdyTeamCreationPolicy::Anyone;
		}

		FString MemberStr;
		if (Obj->TryGetStringField(TEXT("defaultMembershipPolicy"), MemberStr))
			Out.DefaultMembershipPolicy = FCrowdyAppGroupPolicy::MembershipPolicyFromString(MemberStr);

		return true;
	}

	static ECrowdyTeamMembershipPolicy MembershipPolicyFromString(const FString& S)
	{
		if (S == TEXT("open")) return ECrowdyTeamMembershipPolicy::Open;
		if (S == TEXT("request")) return ECrowdyTeamMembershipPolicy::Request;
		if (S == TEXT("invite")) return ECrowdyTeamMembershipPolicy::Invite;
		if (S == TEXT("admin")) return ECrowdyTeamMembershipPolicy::Admin;
		return ECrowdyTeamMembershipPolicy::Open;
	}
};
