#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Magic-link sign-in step 2: complete sign-in with the one-time token carried
 * back on the redirect (or the devToken in dev). This is Public, so the token authorizes
 * the call. Returns the identity SESSION token. Uses an inline query body.
 */
struct FCompleteLoginLinkRequest : ICrowdyQueryRequest
{
	FString Token;

	virtual FName GetOperationName() const override
	{
		return TEXT("Complete Login Link");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::CompleteLoginLink;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		RuntimeVariables.Add(TEXT("token"), Token);

		InlineQueryBody  = GetStaticQueryBody();
		bIncludeAuthToken = false;
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return !Token.IsEmpty();
	}

private:
	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("mutation CompleteLoginLink($token: String!) {")
			TEXT("  completeLoginLink(input: { token: $token }) {")
			TEXT("    token")
			TEXT("    gameTokenId")
			TEXT("    user { userId email }")
			TEXT("  }")
			TEXT("}");
		return Body;
	}
};
