#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamCreationPolicy.h"
#include "Queries/Data/Teams/Enums/ECrowdyTeamMembershipPolicy.h"
#include "Queries/Data/Teams/Types/FCrowdyTeam.h"
#include "FCrowdyTeamPolicy.generated.h"

/** The app-wide rules for teams: who may create one, and the defaults and caps new teams inherit. */
USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyTeamPolicy
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64 AppId = 0;

	UPROPERTY(BlueprintReadOnly) ECrowdyTeamCreationPolicy CreationPolicy = ECrowdyTeamCreationPolicy::Anyone;

	UPROPERTY(BlueprintReadOnly) ECrowdyTeamMembershipPolicy DefaultMembershipPolicy = ECrowdyTeamMembershipPolicy::Open;

	/** Zero means the server set no cap. */
	UPROPERTY(BlueprintReadOnly) int32 MaxMembers = 0;

	/** How many teams one user may belong to. Zero means the server set no cap. */
	UPROPERTY(BlueprintReadOnly) int32 MaxTeamsPerUser = 0;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyTeamPolicy& Out)
	{
		if (!Obj.IsValid()) return false;

		FString AppIdStr;
		if (Obj->TryGetStringField(TEXT("appId"), AppIdStr)) Out.AppId = FCString::Atoi64(*AppIdStr);

		Obj->TryGetNumberField(TEXT("maxMembers"), Out.MaxMembers);
		Obj->TryGetNumberField(TEXT("maxGroupsPerUser"), Out.MaxTeamsPerUser);

		FString CreationStr;
		if (Obj->TryGetStringField(TEXT("creationPolicy"), CreationStr))
			Out.CreationPolicy = CreationPolicyFromString(CreationStr);

		FString MembershipStr;
		if (Obj->TryGetStringField(TEXT("defaultMembershipPolicy"), MembershipStr))
			Out.DefaultMembershipPolicy = FCrowdyTeam::MembershipPolicyFromString(MembershipStr);

		return true;
	}

	static ECrowdyTeamCreationPolicy CreationPolicyFromString(const FString& Value)
	{
		if (Value == TEXT("admin"))  return ECrowdyTeamCreationPolicy::Admin;
		if (Value == TEXT("member")) return ECrowdyTeamCreationPolicy::Member;
		return ECrowdyTeamCreationPolicy::Anyone;
	}

	static FString CreationPolicyToString(ECrowdyTeamCreationPolicy Policy)
	{
		switch (Policy)
		{
		case ECrowdyTeamCreationPolicy::Admin:  return TEXT("admin");
		case ECrowdyTeamCreationPolicy::Member: return TEXT("member");
		case ECrowdyTeamCreationPolicy::Anyone: return TEXT("anyone");
		}
		return TEXT("anyone");
	}
};
