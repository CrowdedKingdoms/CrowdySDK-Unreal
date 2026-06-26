#pragma once
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FGetUserStateResponse : ICrowdyQueryResponse
{
	FString UserStateString;
	int64 UserID;
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::GetUserState;
	}
	
	virtual FName GetOperationName() const override
	{
		return TEXT("Get User State Response");
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("The JSON object is invalid.");
			bIsValid = false;
			return;
		}
		
		const TSharedPtr<FJsonObject>* DataObject;
		const TSharedPtr<FJsonObject>* MeObject;
		
		// Extract data object first
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract data object from response.");
			bIsValid = false;
			return;
		}

		// Extract login object from data
		if (!(*DataObject)->TryGetObjectField(TEXT("me"), MeObject) || !MeObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract user object from response.");
			bIsValid = false;
			return;
		}

		// Extract token from a login object
		if (!(*MeObject)->TryGetStringField(TEXT("state"), UserStateString))
		{
			ErrorMessage = TEXT("Failed to extract state from user object.");
			bIsValid = false;
			return;
		}
		
		FString UserIdString;
		if (!(*MeObject)->TryGetStringField(TEXT("userId"), UserIdString))
		{
			ErrorMessage = TEXT("Failed to extract userId from user object.");
			bIsValid = false;
			return;
		}
		
		UserID = FCString::Atoi64(*UserIdString);
		
		//ErrorMessage = TEXT("User State %s and User ID %lld"), *UserState, UserID));
	}
};