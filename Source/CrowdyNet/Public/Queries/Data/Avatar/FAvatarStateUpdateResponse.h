#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatar.h"

struct FAvatarStateUpdateResponse : ICrowdyQueryResponse
{
	FCrowdyAvatar Avatar;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::UpdateAvatarState; }
	virtual FName GetOperationName() const override { return "Avatar State Update Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid()) { ErrorMessage = TEXT("UpdateAvatarState: Invalid JSON"); return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { ErrorMessage = TEXT("UpdateAvatarState: No 'data' field"); return; }

		const TSharedPtr<FJsonObject>* AvatarObj;
		if (!(*DataObj)->TryGetObjectField(TEXT("updateAvatarState"), AvatarObj)) { ErrorMessage = TEXT("UpdateAvatarState: No 'updateAvatarState' field"); return; }

		if (!FCrowdyAvatar::ParseFromJson(*AvatarObj, Avatar)) { ErrorMessage = TEXT("UpdateAvatarState: Failed to parse avatar"); return; }

		bIsValid = true;
	}
};
