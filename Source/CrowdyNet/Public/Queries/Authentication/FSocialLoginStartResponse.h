#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"

/**
 * Response for the socialLoginStart mutation (social sign-in step 1): the provider
 * authorizes URL to open, and the opaque CSRF state to bind the callback and pass to
 * socialLoginComplete.
 */
struct FSocialLoginStartResponse : ICrowdyQueryResponse
{
	FString AuthorizeUrl;
	FString State;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::SocialLoginStart;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Social Login Start Response");
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
		if (!(*DataObject)->TryGetObjectField(TEXT("socialLoginStart"), RootObject) || !RootObject->IsValid())
		{
			MarkInvalid(TEXT("Missing root field 'socialLoginStart'"));
			return;
		}

		if (!(*RootObject)->TryGetStringField(TEXT("authorizeUrl"), AuthorizeUrl) || AuthorizeUrl.IsEmpty())
		{
			MarkInvalid(TEXT("Missing authorizeUrl"));
			return;
		}
		(*RootObject)->TryGetStringField(TEXT("state"), State);
	}
};
