#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Queries/Authentication/FCrowdyUserIdentity.h"

/** Response for the linkIdentity mutation: the newly linked identity. */
struct FLinkIdentityResponse : ICrowdyQueryResponse
{
	FCrowdyUserIdentity Identity;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::LinkIdentity;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Link Identity Response");
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
		if (!(*DataObject)->TryGetObjectField(TEXT("linkIdentity"), RootObject) || !RootObject->IsValid())
		{
			MarkInvalid(TEXT("Missing root field 'linkIdentity'"));
			return;
		}

		Identity = FCrowdyUserIdentity::FromJson(*RootObject);
		if (Identity.IdentityId.IsEmpty())
		{
			MarkInvalid(TEXT("linkIdentity returned no identityId"));
		}
	}
};
