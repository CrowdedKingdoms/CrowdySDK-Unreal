#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatar.h"

struct FAvatarResponse : ICrowdyQueryResponse
{
	FCrowdyAvatar Avatar;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::GetAvatar; }
	virtual FName GetOperationName() const override { return "Get Avatar Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid()) { ErrorMessage = TEXT("GetAvatar: Invalid JSON"); return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { ErrorMessage = TEXT("GetAvatar: No 'data' field"); return; }

		const TSharedPtr<FJsonObject>* AvatarObj;
		if (!(*DataObj)->TryGetObjectField(TEXT("avatar"), AvatarObj)) { ErrorMessage = TEXT("GetAvatar: No 'avatar' field"); return; }

		if (!FCrowdyAvatar::ParseFromJson(*AvatarObj, Avatar)) { ErrorMessage = TEXT("GetAvatar: Failed to parse avatar"); return; }

		bIsValid = true;
	}
};
