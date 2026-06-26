#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Data/Avatar/Types/FCrowdyAvatar.h"

struct FAvatarCreateResponse : ICrowdyQueryResponse
{
	FCrowdyAvatar Avatar;

	virtual EQueryResponseType GetResponseType() const override { return EQueryResponseType::CreateAvatar; }
	virtual FName GetOperationName() const override { return "Create Avatar Response"; }

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid()) { ErrorMessage = TEXT("CreateAvatar: Invalid JSON"); return; }

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj)) { ErrorMessage = TEXT("CreateAvatar: No 'data' field"); return; }

		const TSharedPtr<FJsonObject>* AvatarObj;
		if (!(*DataObj)->TryGetObjectField(TEXT("createAvatar"), AvatarObj)) { ErrorMessage = TEXT("CreateAvatar: No 'createAvatar' field"); return; }

		if (!FCrowdyAvatar::ParseFromJson(*AvatarObj, Avatar)) { ErrorMessage = TEXT("CreateAvatar: Failed to parse avatar"); return; }

		bIsValid = true;
	}
};
