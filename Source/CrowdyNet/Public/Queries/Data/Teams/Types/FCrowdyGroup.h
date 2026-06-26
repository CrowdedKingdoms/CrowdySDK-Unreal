#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamMembershipPolicy.h"
#include "FCrowdyGroup.generated.h"

USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyGroup
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64   GroupId         = 0;
	UPROPERTY(BlueprintReadOnly) int64   AppId           = 0;
	UPROPERTY(BlueprintReadOnly) FString GroupType;
	UPROPERTY(BlueprintReadOnly) FString Name;
	UPROPERTY(BlueprintReadOnly) FString Description;
	UPROPERTY(BlueprintReadOnly) int64   OwnerUserId     = 0;
	UPROPERTY(BlueprintReadOnly) ECrowdyTeamMembershipPolicy MembershipPolicy = ECrowdyTeamMembershipPolicy::Open;
	UPROPERTY(BlueprintReadOnly) FString Status;
	UPROPERTY(BlueprintReadOnly) FString CreatedAt;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyGroup& Out)
	{
		if (!Obj.IsValid()) return false;

		FString IdStr;
		if (Obj->TryGetStringField(TEXT("groupId"), IdStr))   Out.GroupId     = FCString::Atoi64(*IdStr);
		FString AidStr;
		if (Obj->TryGetStringField(TEXT("appId"), AidStr))    Out.AppId       = FCString::Atoi64(*AidStr);
		FString OidStr;
		if (Obj->TryGetStringField(TEXT("ownerUserId"), OidStr)) Out.OwnerUserId = FCString::Atoi64(*OidStr);

		Obj->TryGetStringField(TEXT("groupType"),  Out.GroupType);
		Obj->TryGetStringField(TEXT("name"),       Out.Name);
		Obj->TryGetStringField(TEXT("description"), Out.Description);
		Obj->TryGetStringField(TEXT("status"),     Out.Status);
		Obj->TryGetStringField(TEXT("createdAt"),  Out.CreatedAt);

		FString PolicyStr;
		if (Obj->TryGetStringField(TEXT("membershipPolicy"), PolicyStr))
			Out.MembershipPolicy = MembershipPolicyFromString(PolicyStr);

		return true;
	}

	static ECrowdyTeamMembershipPolicy MembershipPolicyFromString(const FString& S)
	{
		if (S == TEXT("open"))    return ECrowdyTeamMembershipPolicy::Open;
		if (S == TEXT("request")) return ECrowdyTeamMembershipPolicy::Request;
		if (S == TEXT("invite"))  return ECrowdyTeamMembershipPolicy::Invite;
		if (S == TEXT("admin"))   return ECrowdyTeamMembershipPolicy::Admin;
		return ECrowdyTeamMembershipPolicy::Open;
	}

	static FString MembershipPolicyToString(ECrowdyTeamMembershipPolicy P)
	{
		switch (P)
		{
		case ECrowdyTeamMembershipPolicy::Open:    return TEXT("open");
		case ECrowdyTeamMembershipPolicy::Request: return TEXT("request");
		case ECrowdyTeamMembershipPolicy::Invite:  return TEXT("invite");
		case ECrowdyTeamMembershipPolicy::Admin:   return TEXT("admin");
		}
		return TEXT("open");
	}
};
