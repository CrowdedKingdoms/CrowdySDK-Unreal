#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "FCrowdyAppAvatarState.generated.h"

USTRUCT(BlueprintType)
struct CROWDYNET_API FCrowdyAppAvatarState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Avatars") int64   AppId   = 0;
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Avatars") int64   AvatarId= 0;
	/** Base64-encoded binary state for this app. Empty string means not set. Use DeserializeFromAvatarState to decode. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Avatars") FString RawState;
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Avatars") FString CreatedAt;
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Avatars") FString UpdatedAt;

	static bool ParseFromJson(const TSharedPtr<FJsonObject>& Obj, FCrowdyAppAvatarState& Out)
	{
		if (!Obj.IsValid()) return false;

		FString AppIdStr;
		if (Obj->TryGetStringField(TEXT("appId"), AppIdStr))
			Out.AppId = FCString::Atoi64(*AppIdStr);

		FString AvatarIdStr;
		if (Obj->TryGetStringField(TEXT("avatarId"), AvatarIdStr))
			Out.AvatarId = FCString::Atoi64(*AvatarIdStr);

		Obj->TryGetStringField(TEXT("state"), Out.RawState);
		Obj->TryGetStringField(TEXT("createdAt"), Out.CreatedAt);
		Obj->TryGetStringField(TEXT("updatedAt"), Out.UpdatedAt);

		return Out.AppId != 0 && Out.AvatarId != 0;
	}
};
