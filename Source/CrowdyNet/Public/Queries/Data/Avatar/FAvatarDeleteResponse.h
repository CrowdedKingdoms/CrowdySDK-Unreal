#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatar.h"

struct FAvatarDeleteResponse : ICrowdyQueryResponse
{
	FCrowdyAvatar Avatar;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::DeleteAvatar; }
	virtual FName GetOperationName() const override { return "Delete Avatar Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid()) { ErrorMessage = TEXT("DeleteAvatar: Invalid JSON"); return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { ErrorMessage = TEXT("DeleteAvatar: No 'data' field"); return; }

		const TSharedPtr<FJsonObject>* AvatarObj;
		if (!(*DataObj)->TryGetObjectField(TEXT("deleteAvatar"), AvatarObj)) { ErrorMessage = TEXT("DeleteAvatar: No 'deleteAvatar' field"); return; }

		if (!FCrowdyAvatar::ParseFromJson(*AvatarObj, Avatar)) { ErrorMessage = TEXT("DeleteAvatar: Failed to parse avatar"); return; }

		bIsValid = true;
	}
};
