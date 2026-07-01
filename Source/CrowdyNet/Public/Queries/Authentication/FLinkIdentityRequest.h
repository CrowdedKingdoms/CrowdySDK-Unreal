#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Link an additional federated identity (from a socialLoginStart callback) to the
 * signed-in account. Requires the SESSION token; the server throws if the identity
 * is already linked to another account. Same code+state pair as socialLoginComplete,
 * but links to the current account instead of starting a session. Inline body.
 */
struct FLinkIdentityRequest : ICrowdyQueryRequest
{
	FString Provider;
	FString Code;
	FString State;

	virtual FName GetOperationName() const override
	{
		return TEXT("Link Identity");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::LinkIdentity;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		RuntimeVariables.Add(TEXT("provider"), Provider);
		RuntimeVariables.Add(TEXT("code"),     Code);
		RuntimeVariables.Add(TEXT("state"),    State);

		InlineQueryBody   = GetStaticQueryBody();
		bIncludeAuthToken = true; // session token, attached by descriptor scope
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return !Provider.IsEmpty() && !Code.IsEmpty() && !State.IsEmpty();
	}

private:
	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("mutation LinkIdentity($provider: String!, $code: String!, $state: String!) {")
			TEXT("  linkIdentity(input: { provider: $provider, code: $code, state: $state }) {")
			TEXT("    identityId")
			TEXT("    userId")
			TEXT("    provider")
			TEXT("    subject")
			TEXT("    email")
			TEXT("    emailVerified")
			TEXT("    createdAt")
			TEXT("    lastLoginAt")
			TEXT("  }")
			TEXT("}");
		return Body;
	}
};
