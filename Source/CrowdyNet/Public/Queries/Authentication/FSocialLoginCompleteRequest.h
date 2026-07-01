#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Social sign-in step 2: complete a federated sign-in from the provider callback
 * (code and state). This is public and the one-time code authorizes the call. Returns the
 * identity SESSION token (AuthResponse), creating or linking the account by provider
 * identity. Uses an inline query body.
 */
struct FSocialLoginCompleteRequest : ICrowdyQueryRequest
{
	FString Provider;
	FString Code;
	FString State;

	virtual FName GetOperationName() const override
	{
		return TEXT("Social Login Complete");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::SocialLoginComplete;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		RuntimeVariables.Add(TEXT("provider"), Provider);
		RuntimeVariables.Add(TEXT("code"),     Code);
		RuntimeVariables.Add(TEXT("state"),    State);

		InlineQueryBody   = GetStaticQueryBody();
		bIncludeAuthToken = false;
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
			TEXT("mutation SocialLoginComplete($provider: String!, $code: String!, $state: String!) {")
			TEXT("  socialLoginComplete(input: { provider: $provider, code: $code, state: $state }) {")
			TEXT("    token")
			TEXT("    gameTokenId")
			TEXT("    user { userId email }")
			TEXT("  }")
			TEXT("}");
		return Body;
	}
};
