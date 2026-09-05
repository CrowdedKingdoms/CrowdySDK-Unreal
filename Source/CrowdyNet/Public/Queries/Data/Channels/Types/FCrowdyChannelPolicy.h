#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Channels/Enums/ECrowdyChannelCreationPolicy.h"
#include "Queries/Data/Channels/Enums/ECrowdyChannelMembershipPolicy.h"
#include "Queries/Data/Channels/Types/FCrowdyChannel.h"
#include "FCrowdyChannelPolicy.generated.h"

/** The app-wide rules for channels: who may create one, and the defaults and caps new channels inherit. */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyChannelPolicy
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64 AppId = 0;

	UPROPERTY(BlueprintReadOnly) ECrowdyChannelCreationPolicy CreationPolicy = ECrowdyChannelCreationPolicy::Anyone;

	UPROPERTY(BlueprintReadOnly)
	ECrowdyChannelMembershipPolicy DefaultMembershipPolicy = ECrowdyChannelMembershipPolicy::Open;

	/** Zero means the server set no cap. */
	UPROPERTY(BlueprintReadOnly) int32 MaxMembers = 0;

	/** How many channels one user may belong to. Zero means the server set no cap. */
	UPROPERTY(BlueprintReadOnly) int32 MaxChannelsPerUser = 0;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyChannelPolicy& Out)
	{
		if (!Obj.IsValid()) return false;

		FString AppIdStr;
		if (Obj->TryGetStringField(TEXT("appId"), AppIdStr)) Out.AppId = FCString::Atoi64(*AppIdStr);

		Obj->TryGetNumberField(TEXT("maxMembers"), Out.MaxMembers);
		Obj->TryGetNumberField(TEXT("maxGroupsPerUser"), Out.MaxChannelsPerUser);

		FString CreationStr;
		if (Obj->TryGetStringField(TEXT("creationPolicy"), CreationStr))
			Out.CreationPolicy = CreationPolicyFromString(CreationStr);

		FString MembershipStr;
		if (Obj->TryGetStringField(TEXT("defaultMembershipPolicy"), MembershipStr))
			Out.DefaultMembershipPolicy = FCrowdyChannel::MembershipPolicyFromString(MembershipStr);

		return true;
	}

	static ECrowdyChannelCreationPolicy CreationPolicyFromString(const FString& Value)
	{
		if (Value == TEXT("admin"))  return ECrowdyChannelCreationPolicy::Admin;
		if (Value == TEXT("member")) return ECrowdyChannelCreationPolicy::Member;
		return ECrowdyChannelCreationPolicy::Anyone;
	}

	static FString CreationPolicyToString(ECrowdyChannelCreationPolicy Policy)
	{
		switch (Policy)
		{
		case ECrowdyChannelCreationPolicy::Admin:  return TEXT("admin");
		case ECrowdyChannelCreationPolicy::Member: return TEXT("member");
		case ECrowdyChannelCreationPolicy::Anyone: return TEXT("anyone");
		}
		return TEXT("anyone");
	}
};
