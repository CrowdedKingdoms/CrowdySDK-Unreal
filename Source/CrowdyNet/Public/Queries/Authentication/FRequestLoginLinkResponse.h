#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

/**
 * Response for the requestLoginLink mutation (magic-link sign-in step 1).
 * sent is always true (no account enumeration). devToken is present only when
 * the server runs with DEV_AUTH_BYPASS. This allows tests/local flows to complete
 * sign-in without an inbox; null in production.
 */
struct FRequestLoginLinkResponse : ICrowdyQueryResponse
{
	bool    bSent = false;
	FString DevToken;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::RequestLoginLink;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Request Login Link Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			MarkInvalid(TEXT("Invalid JSON Object"));
			return;
		}

		const TSharedPtr<FJsonObject>* DataObject = nullptr;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			MarkInvalid(TEXT("Invalid Data JSON Object"));
			return;
		}

		const TSharedPtr<FJsonObject>* RootObject = nullptr;
		if (!(*DataObject)->TryGetObjectField(TEXT("requestLoginLink"), RootObject) || !RootObject->IsValid())
		{
			MarkInvalid(TEXT("Missing root field 'requestLoginLink'"));
			return;
		}

		(*RootObject)->TryGetBoolField(TEXT("sent"), bSent);
		(*RootObject)->TryGetStringField(TEXT("devToken"), DevToken); // null in production
	}
};
