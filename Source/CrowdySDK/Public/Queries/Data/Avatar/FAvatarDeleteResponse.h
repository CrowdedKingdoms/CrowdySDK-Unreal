#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FAvatarDeleteResponse : ICrowdyQueryResponse
{
	FString DeletedAvatarName;
	
	virtual EQueryResponseType GetResponseType() const override
	{
		 return EQueryResponseType::DeleteAvatar;
	}
	
	virtual FName GetOperationName() const override
	{
		return "Delete Avatar Response";
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;
		
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("Delete Avatar Response: Invalid Json Object");
			return;
		}
		
		const TSharedPtr<FJsonObject>* DataObject;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("HandleDeleteAvatarResponse: No data field in response");
			return;
		}
		
		// Get the deleteAvatar object
		const TSharedPtr<FJsonObject>* DeleteAvatarObject;
		if (!(*DataObject)->TryGetObjectField(TEXT("deleteAvatar"), DeleteAvatarObject))
		{
			ErrorMessage = TEXT("HandleDeleteAvatarResponse: No deleteAvatar field in data");
			return;
		}
		
		if (!(*DeleteAvatarObject)->TryGetStringField(TEXT("name"), DeletedAvatarName))
		{
			ErrorMessage = TEXT("HandleDeleteAvatarResponse: Invalid response");
			return;
		}

		if (DeletedAvatarName.IsEmpty())
		{
			ErrorMessage = TEXT("HandleDeleteAvatarResponse: Empty name");
			return;
		}
		
		bIsValid = true;
	}
};