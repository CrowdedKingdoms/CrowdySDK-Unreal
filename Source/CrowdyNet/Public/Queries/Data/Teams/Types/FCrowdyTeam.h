#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamMembershipPolicy.h"
#include "FCrowdyTeam.generated.h"

/** One team, as the server describes it. */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyTeam
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64   TeamId      = 0;
	UPROPERTY(BlueprintReadOnly) int64   AppId       = 0;
	UPROPERTY(BlueprintReadOnly) FString Name;
	UPROPERTY(BlueprintReadOnly) FString Description;
	UPROPERTY(BlueprintReadOnly) int64   OwnerUserId = 0;
	UPROPERTY(BlueprintReadOnly) ECrowdyTeamMembershipPolicy MembershipPolicy = ECrowdyTeamMembershipPolicy::Open;
	UPROPERTY(BlueprintReadOnly) FString Status;
	UPROPERTY(BlueprintReadOnly) FString CreatedAt;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyTeam& Out)
	{
		if (!Obj.IsValid()) return false;

		// The ids arrive as BigInt JSON strings.
		FString IdStr;
		if (Obj->TryGetStringField(TEXT("groupId"), IdStr))      Out.TeamId      = FCString::Atoi64(*IdStr);
		FString AppIdStr;
		if (Obj->TryGetStringField(TEXT("appId"), AppIdStr))     Out.AppId       = FCString::Atoi64(*AppIdStr);
		FString OwnerStr;
		if (Obj->TryGetStringField(TEXT("ownerUserId"), OwnerStr)) Out.OwnerUserId = FCString::Atoi64(*OwnerStr);

		Obj->TryGetStringField(TEXT("name"),        Out.Name);
		Obj->TryGetStringField(TEXT("description"), Out.Description);
		Obj->TryGetStringField(TEXT("status"),      Out.Status);
		Obj->TryGetStringField(TEXT("createdAt"),   Out.CreatedAt);

		FString PolicyStr;
		if (Obj->TryGetStringField(TEXT("membershipPolicy"), PolicyStr))
			Out.MembershipPolicy = MembershipPolicyFromString(PolicyStr);

		return true;
	}

	static ECrowdyTeamMembershipPolicy MembershipPolicyFromString(const FString& Value)
	{
		if (Value == TEXT("open"))    return ECrowdyTeamMembershipPolicy::Open;
		if (Value == TEXT("request")) return ECrowdyTeamMembershipPolicy::Request;
		if (Value == TEXT("invite"))  return ECrowdyTeamMembershipPolicy::Invite;
		if (Value == TEXT("admin"))   return ECrowdyTeamMembershipPolicy::Admin;
		return ECrowdyTeamMembershipPolicy::Open;
	}

	static FString MembershipPolicyToString(ECrowdyTeamMembershipPolicy Policy)
	{
		switch (Policy)
		{
		case ECrowdyTeamMembershipPolicy::Open:    return TEXT("open");
		case ECrowdyTeamMembershipPolicy::Request: return TEXT("request");
		case ECrowdyTeamMembershipPolicy::Invite:  return TEXT("invite");
		case ECrowdyTeamMembershipPolicy::Admin:   return TEXT("admin");
		}
		return TEXT("open");
	}
};
