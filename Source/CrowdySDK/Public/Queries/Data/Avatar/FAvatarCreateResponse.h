#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FAvatarCreateResponse : ICrowdyQueryResponse
{
	
	FString AvatarName;
	int64 AvatarID;
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::CreateAvatar;
	}
	
	virtual FName GetOperationName() const override
	{
		return "Create Avatar Response";
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("Json object is invalid");
			bIsValid = false;
			return;
		}
		
		const TSharedPtr<FJsonObject>* DataObject;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject))
		{
			ErrorMessage = TEXT("Create Avatar: No 'data' field in the response");
			bIsValid = false;
			return;
		}
		
		const TSharedPtr<FJsonObject>* CreateAvatarObject;
		if (!(*DataObject)->TryGetObjectField(TEXT("createAvatar"), CreateAvatarObject))
		{
			ErrorMessage = TEXT("Create Avatar: No 'createAvatar' field in data");
			bIsValid = false;
			return;
		}
		
		FString AvatarIdString;
		
		if (!(*CreateAvatarObject)->TryGetStringField(TEXT("avatarId"), AvatarIdString))
		{
			ErrorMessage = TEXT("CreateAvatar: Missing avatarId field");
			bIsValid = false;
			return;
		}
		
		AvatarID = FCString::Atoi64(*AvatarIdString);
		
		if (!(*CreateAvatarObject)->TryGetStringField(TEXT("name"), AvatarName))
		{
			ErrorMessage = TEXT("CreateAvatar: Missing name field");
			bIsValid = false;
			return;
		}
		
		bIsValid = true;
	}
};