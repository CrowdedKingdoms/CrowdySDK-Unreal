#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FAvatarStateUpdateResponse : ICrowdyQueryResponse
{
	FString UpdatedAvatarState;
	int64 AvatarID;
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::UpdateAvatarState;
	}
	
	virtual FName GetOperationName() const override
	{
		return FName("Update Avatar State Response");
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;
		
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("AvatarStateUpdateResponse: Json Object is invalid.");
			return;
		}
		
		// Check if the "data" field exists
		const TSharedPtr<FJsonObject>* DataObject;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject))
		{
			ErrorMessage =  TEXT("UpdateAvatarState: No 'data' field in response");
			return;
		}
		
		// Check if the "updateAvatarState" field exists within data
		const TSharedPtr<FJsonObject>* UpdateAvatarStateObject;
		if (!(*DataObject)->TryGetObjectField(TEXT("updateAvatarState"), UpdateAvatarStateObject))
		{
			ErrorMessage =  TEXT("UpdateAvatarState: No 'updateAvatarState' field in data");
			return;
		}
		
		// Extract publicState
		if (!(*UpdateAvatarStateObject)->TryGetStringField(TEXT("publicState"), UpdatedAvatarState))
		{
			ErrorMessage = TEXT("UpdateAvatarState: Missing publicState field");
			return;
		}
		
		if (UpdatedAvatarState.IsEmpty())
		{
			ErrorMessage = TEXT("UpdateAvatarState: Empty updated Avatar State");
			return;
		}
		
		FString AvatarIdString;
		if (!(*UpdateAvatarStateObject)->TryGetStringField(TEXT("avatarId"), AvatarIdString))
		{
			ErrorMessage = TEXT("UpdateAvatarState: Missing avatarId field");
			return;
		}
		
		if (AvatarIdString.IsEmpty())
		{
			ErrorMessage = TEXT("UpdateAvatarState: Empty Avatar Id");
			return;
		}
		
		AvatarID = FCString::Atoi64(*AvatarIdString);
		bIsValid = true;
	}
};