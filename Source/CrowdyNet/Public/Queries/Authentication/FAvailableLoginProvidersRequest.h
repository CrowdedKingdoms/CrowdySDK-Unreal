#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Query the federated sign-in providers currently enabled (e.g. ['google']). Public
 * (no auth); drives the sign-in UI so provider buttons are not hard-coded. The dev
 * mock provider appears only when the server dev bypass is enabled. Inline body, no
 * variables.
 */
struct FAvailableLoginProvidersRequest : ICrowdyQueryRequest
{
	virtual FName GetOperationName() const override
	{
		return TEXT("Available Login Providers");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::AvailableLoginProviders;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();

		InlineQueryBody   = GetStaticQueryBody();
		bIncludeAuthToken = false;
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return true;
	}

private:
	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("query AvailableLoginProviders {")
			TEXT("  availableLoginProviders")
			TEXT("}");
		return Body;
	}
};
