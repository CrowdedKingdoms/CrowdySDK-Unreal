#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FRegisterResponse : ICrowdyQueryResponse
{
	FString GameToken;
	int64 UserID;
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::Register;
	}
	
	virtual FName GetOperationName() const override
	{
		return "Register Response";
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("The JSON Object is invalid.");
			bIsValid = false;
			return;
		}
		
		const TSharedPtr<FJsonObject>* DataObject;
		const TSharedPtr<FJsonObject>* RegisterObject;
		const TSharedPtr<FJsonObject>* UserObject;
		
		// Extract data object first
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract data object from response.");
			bIsValid = false;
			return;
		}

		// Extract login object from data
		if (!(*DataObject)->TryGetObjectField(TEXT("register"), RegisterObject) || !RegisterObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract login object from response.");
			bIsValid = false;
			return;
		}

		// Extract token from a register object
		if (!(*RegisterObject)->TryGetStringField(TEXT("token"), GameToken))
		{
			UE_LOG(LogTemp, Warning, TEXT("Token not found in login object or failed to extract token"));
			ErrorMessage = TEXT("Failed to extract login object from response.");
			bIsValid = false;
			return;
		}
		
		// Extract user object from register object
		if ((*RegisterObject)->TryGetObjectField(TEXT("user"), UserObject) && UserObject->IsValid())
		{
			FString UserIdString;
			
			if ((*UserObject)->TryGetStringField(TEXT("userId"), UserIdString))
			{
				UserID = FCString::Atoi64(*UserIdString);
			}
			else
			{
				ErrorMessage = TEXT("Failed to extract userId from response.");
				bIsValid = false;
			}
		}
		else
		{
			ErrorMessage = TEXT("Failed to extract login object from response.");
			bIsValid = false;
			return;
		}
		
		bIsValid = true;
	}
};