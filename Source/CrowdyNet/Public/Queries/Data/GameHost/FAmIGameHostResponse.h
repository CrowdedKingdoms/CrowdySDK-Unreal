#pragma once
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

// Response for the `amIGameHost(appId): Boolean!` query. Authoritative, server-side
// answer to "is the authenticated caller (this client) the elected host for the app".
struct FAmIGameHostResponse : ICrowdyQueryResponse
{
	bool bAmHost = false;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::AmIGameHost;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Am I Game Host Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;
		bAmHost  = false;

		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("AmIGameHost: Invalid JSON object.");
			return;
		}

		const TSharedPtr<FJsonObject>* DataObject;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("AmIGameHost: Missing 'data' field.");
			return;
		}

		if (!(*DataObject)->TryGetBoolField(TEXT("amIGameHost"), bAmHost))
		{
			ErrorMessage = TEXT("AmIGameHost: Missing 'amIGameHost' field in data.");
			return;
		}

		bIsValid = true;
	}
};
