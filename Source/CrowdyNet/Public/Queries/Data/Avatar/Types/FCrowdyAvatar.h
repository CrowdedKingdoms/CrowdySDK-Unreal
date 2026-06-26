#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "FCrowdyAvatar.generated.h"

USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyAvatar
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly) int64   AvatarId    = 0;
	UPROPERTY(BlueprintReadOnly) int64   UserId      = 0;
	UPROPERTY(BlueprintReadOnly) FString Name;
	/** Base64-encoded binary state visible to all players. */
	UPROPERTY(BlueprintReadOnly) FString PublicState;
	/** Base64-encoded binary state visible to the owner only. Empty for non-owner lookups. */
	UPROPERTY(BlueprintReadOnly) FString PrivateState;
	UPROPERTY(BlueprintReadOnly) FString CreatedAt;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyAvatar& Out)
	{
		if (!Obj.IsValid()) return false;

		FString IdStr;
		if (Obj->TryGetStringField(TEXT("avatarId"), IdStr))
			Out.AvatarId = FCString::Atoi64(*IdStr);

		FString UserIdStr;
		if (Obj->TryGetStringField(TEXT("userId"), UserIdStr))
			Out.UserId = FCString::Atoi64(*UserIdStr);

		Obj->TryGetStringField(TEXT("name"), Out.Name);
		Obj->TryGetStringField(TEXT("publicState"), Out.PublicState);
		Obj->TryGetStringField(TEXT("privateState"), Out.PrivateState);
		Obj->TryGetStringField(TEXT("createdAt"), Out.CreatedAt);

		return Out.AvatarId != 0;
	}
};
