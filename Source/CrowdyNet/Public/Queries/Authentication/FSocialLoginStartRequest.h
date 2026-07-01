#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Social sign-in step 1: begin a federated (OAuth) sign-in. Public (no auth).
 * Returns an authorizeUrl to open in the browser and an opaque state to round-trip
 * back to socialLoginComplete (CSRF binding). RedirectUri is the loopback the
 * provider returns to (http://127.0.0.1:<port>/callback) and must be a registered
 * callback for the app. Uses an inline query body.
 */
struct FSocialLoginStartRequest : ICrowdyQueryRequest
{
	FString Provider;
	FString RedirectUri;

	virtual FName GetOperationName() const override
	{
		return TEXT("Social Login Start");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::SocialLoginStart;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		RuntimeVariables.Add(TEXT("provider"),    Provider);
		RuntimeVariables.Add(TEXT("redirectUri"), RedirectUri);

		InlineQueryBody   = GetStaticQueryBody();
		bIncludeAuthToken = false;
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return !Provider.IsEmpty() && !RedirectUri.IsEmpty();
	}

private:
	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("mutation SocialLoginStart($provider: String!, $redirectUri: String!) {")
			TEXT("  socialLoginStart(input: { provider: $provider, redirectUri: $redirectUri }) {")
			TEXT("    authorizeUrl")
			TEXT("    state")
			TEXT("  }")
			TEXT("}");
		return Body;
	}
};
