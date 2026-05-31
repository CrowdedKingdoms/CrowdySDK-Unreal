#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FAvatarNameUpdateResponse : ICrowdyQueryResponse
{
	FString UpdatedAvatarName;
	int64 AvatarID;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::UpdateAvatar;
	}

	virtual FName GetOperationName() const override
	{
		return "Avatar Name Update Response";
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		// Check if Response is valid
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("UpdateAvatarName: Invalid response received");
			return;
		}
		
		// Check if the "data" field exists
		const TSharedPtr<FJsonObject>* DataObject;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject))
		{
			ErrorMessage = TEXT("UpdateAvatarName: No 'data' field in response");
			return;
		}
		
		// Check if the "updateAvatar" field exists within data
		const TSharedPtr<FJsonObject>* UpdateAvatarObject;
		if (!(*DataObject)->TryGetObjectField(TEXT("updateAvatar"), UpdateAvatarObject))
		{
			ErrorMessage =  TEXT("UpdateAvatarName: No 'updateAvatar' field in data");
			return;
		}
		
		// Extract avatarId
		FString AvatarIdString;
		if (!(*UpdateAvatarObject)->TryGetStringField(TEXT("avatarId"), AvatarIdString) || AvatarIdString.IsEmpty())
		{
			ErrorMessage =  TEXT("UpdateAvatarName: Missing or empty avatarId field");
			return;
		}
		
		if (!(*UpdateAvatarObject)->TryGetStringField(TEXT("name"), UpdatedAvatarName) || UpdatedAvatarName.IsEmpty())
		{
			ErrorMessage =  TEXT("UpdateAvatarName: Missing or empty name field");
			return;
		}
		
		AvatarID = FCString::Atoi64(*AvatarIdString);
		bIsValid = true;
	}
};
