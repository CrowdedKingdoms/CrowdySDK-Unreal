#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Channels/Enums/ECrowdyChannelMembershipPolicy.h"
#include "FCrowdyChannel.generated.h"

/** One channel, as the server describes it. */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyChannel
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64   ChannelId   = 0;
	UPROPERTY(BlueprintReadOnly) int64   AppId       = 0;
	UPROPERTY(BlueprintReadOnly) FString Name;
	UPROPERTY(BlueprintReadOnly) FString Description;
	UPROPERTY(BlueprintReadOnly) int64   OwnerUserId = 0;
	UPROPERTY(BlueprintReadOnly) ECrowdyChannelMembershipPolicy MembershipPolicy = ECrowdyChannelMembershipPolicy::Open;
	UPROPERTY(BlueprintReadOnly) FString Status;
	UPROPERTY(BlueprintReadOnly) FString CreatedAt;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyChannel& Out)
	{
		if (!Obj.IsValid()) return false;

		// The ids arrive as BigInt JSON strings.
		FString IdStr;
		if (Obj->TryGetStringField(TEXT("groupId"), IdStr))      Out.ChannelId   = FCString::Atoi64(*IdStr);
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

	static ECrowdyChannelMembershipPolicy MembershipPolicyFromString(const FString& Value)
	{
		if (Value == TEXT("open"))    return ECrowdyChannelMembershipPolicy::Open;
		if (Value == TEXT("request")) return ECrowdyChannelMembershipPolicy::Request;
		if (Value == TEXT("invite"))  return ECrowdyChannelMembershipPolicy::Invite;
		if (Value == TEXT("admin"))   return ECrowdyChannelMembershipPolicy::Admin;
		return ECrowdyChannelMembershipPolicy::Open;
	}

	static FString MembershipPolicyToString(ECrowdyChannelMembershipPolicy Policy)
	{
		switch (Policy)
		{
		case ECrowdyChannelMembershipPolicy::Open:    return TEXT("open");
		case ECrowdyChannelMembershipPolicy::Request: return TEXT("request");
		case ECrowdyChannelMembershipPolicy::Invite:  return TEXT("invite");
		case ECrowdyChannelMembershipPolicy::Admin:   return TEXT("admin");
		}
		return TEXT("open");
	}
};
