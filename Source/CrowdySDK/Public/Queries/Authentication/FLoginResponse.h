#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FLoginResponse : ICrowdyQueryResponse
{
	FString GameToken;
	int64 GameTokenID;
	int64 UserID;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::Login;
	}
	virtual FName GetOperationName() const override
	{
		return "Login Response";
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			ErrorMessage = FString::Printf(TEXT("Invalid JSON Object"));
			bIsValid = false;
			return;
		}
		
		const TSharedPtr<FJsonObject>* DataObject;
		const TSharedPtr<FJsonObject>* LoginObject;
		const TSharedPtr<FJsonObject>* UserObject;
		
		
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = FString::Printf(TEXT("Invalid Data JSON Object"));
			bIsValid = false;
			return;
		}
		
		// Extract login object from data
		if (!(*DataObject)->TryGetObjectField(TEXT("login"), LoginObject) || !LoginObject->IsValid())
		{
			ErrorMessage = FString::Printf(TEXT("Invalid Login JSON Object"));
			bIsValid = false;
			return;
		}

		// Extract token from a login object
		if (!(*LoginObject)->TryGetStringField(TEXT("token"), GameToken))
		{
			ErrorMessage = FString::Printf(TEXT("Invalid Token JSON Object"));
			bIsValid = false;
			return;
		}
		
		FString GameTokenIDStr;
		
		if (!(*LoginObject)->TryGetStringField(TEXT("gameTokenId"), GameTokenIDStr))
		{
			ErrorMessage = FString::Printf(TEXT("Invalid GameToken JSON Object"));
			bIsValid = false;
			return;
		}
		
		if (!GameTokenIDStr.IsEmpty())
		{
			GameTokenID = FCString::Atoi64(*GameTokenIDStr);
		}
		else
		{
			ErrorMessage = FString::Printf(TEXT("Invalid GameToken ID"));
			bIsValid = false;
			return;
		}
		
		// Extract the user object from the login object
		if ((*LoginObject)->TryGetObjectField(TEXT("user"), UserObject) && UserObject->IsValid())
		{
			FString UserIdString;
			if ((*UserObject)->TryGetStringField(TEXT("userId"), UserIdString))
			{
				UserID = FCString::Atoi64(*UserIdString);
			}
			else
			{
				ErrorMessage = FString::Printf(TEXT("Invalid User ID"));
				bIsValid = false;
			}
		}
		else
		{
			ErrorMessage = FString::Printf(TEXT("Invalid User ID"));
			bIsValid = false;
		}
		
	}
	
};