#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Magic-link sign-in step 1: email a one-time link. Public (no auth). The
 * RedirectUri is the native loopback/scheme the OS hands back to (e.g.
 * http://127.0.0.1:<port>/callback); it is omitted when empty so the server
 * uses its default sign-in page. Uses an inline query body so there is no data-asset row.
 */
struct FRequestLoginLinkRequest : ICrowdyQueryRequest
{
	FString Email;
	FString RedirectUri;

	virtual FName GetOperationName() const override
	{
		return TEXT("Request Login Link");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::RequestLoginLink;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		RuntimeVariables.Add(TEXT("email"), Email);
		if (!RedirectUri.IsEmpty())
		{
			RuntimeVariables.Add(TEXT("redirectUri"), RedirectUri);
		}

		InlineQueryBody  = GetStaticQueryBody();
		bIncludeAuthToken = false;
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return !Email.IsEmpty();
	}

private:
	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("mutation RequestLoginLink($email: String!, $redirectUri: String) {")
			TEXT("  requestLoginLink(input: { email: $email, redirectUri: $redirectUri }) {")
			TEXT("    sent")
			TEXT("    devToken")
			TEXT("  }")
			TEXT("}");
		return Body;
	}
};
