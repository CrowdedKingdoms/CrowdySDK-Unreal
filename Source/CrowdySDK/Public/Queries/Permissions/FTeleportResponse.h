#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

struct FTeleportResponse : ICrowdyQueryResponse
{
	bool bTeleportAllowed = false;
	
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::TeleportRequest;
	}
	
	virtual FName GetOperationName() const override
	{
		return "Teleport Response";
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
		const TSharedPtr<FJsonObject>* TeleportRequestObject;

		// Extract data object first
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract data object from response.");
			bIsValid = false;
			return;
		}

		// Extract teleport response object
		if (!(*DataObject)->TryGetObjectField(TEXT("teleportRequest"), TeleportRequestObject) || !TeleportRequestObject->
			IsValid())
		{
			ErrorMessage = TEXT("Failed to extract teleportRequest from response.");
			bIsValid = false;
			return;
		}

		bTeleportAllowed = (*TeleportRequestObject)->TryGetField(TEXT("success"))->AsBool();
		bIsValid = true;
	}
	
};