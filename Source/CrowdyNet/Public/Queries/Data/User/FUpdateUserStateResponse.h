#pragma once
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FUpdateUserStateResponse : ICrowdyQueryResponse
{
	int64 UserID;
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::UpdateUserState;
	}
	
	virtual FName GetOperationName() const override
	{
		return "Update User State Response";
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("Invalid Json Object");
			bIsValid = false;
			return;
		}
		
		const TSharedPtr<FJsonObject>* DataObject;
		const TSharedPtr<FJsonObject>* UpdateUserStateObject;

		// Extract data object first
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract data object from response");
			bIsValid = false;
			return;
		}

		// Extract login object from data
		if (!(*DataObject)->TryGetObjectField(TEXT("updateUserState"), UpdateUserStateObject) || !UpdateUserStateObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract user object from response");
			bIsValid = false;
			return;
		}

		FString UserIdString;
		
		if (!(*UpdateUserStateObject)->TryGetStringField(TEXT("userId"), UserIdString))
		{
			ErrorMessage = TEXT("Failed to extract userId from user object.");
			bIsValid = false;
			return;
		}
		
		UserID = FCString::Atoi64(*UserIdString);
	}
};