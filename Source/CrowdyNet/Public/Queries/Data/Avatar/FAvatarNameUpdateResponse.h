#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatar.h"

struct FAvatarNameUpdateResponse : ICrowdyQueryResponse
{
	FCrowdyAvatar Avatar;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::UpdateAvatar; }
	virtual FName GetOperationName() const override { return "Avatar Name Update Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid()) { ErrorMessage = TEXT("UpdateAvatar: Invalid JSON"); return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { ErrorMessage = TEXT("UpdateAvatar: No 'data' field"); return; }

		const TSharedPtr<FJsonObject>* AvatarObj;
		if (!(*DataObj)->TryGetObjectField(TEXT("updateAvatar"), AvatarObj)) { ErrorMessage = TEXT("UpdateAvatar: No 'updateAvatar' field"); return; }

		if (!FCrowdyAvatar::ParseFromJson(*AvatarObj, Avatar)) { ErrorMessage = TEXT("UpdateAvatar: Failed to parse avatar"); return; }

		bIsValid = true;
	}
};
