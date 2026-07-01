#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Rotate the current app-scoped token for the SAME app before it expires. Bearer
 * = the CURRENT app token (not the session token), so the descriptor pins this
 * to the App token scope even though it is a Management-plane mutation. Takes no
 * input. Uses an inline query body.
 */
struct FRefreshAppTokenRequest : ICrowdyQueryRequest
{
	virtual FName GetOperationName() const override
	{
		return TEXT("Refresh App Token");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::RefreshAppToken;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();

		InlineQueryBody  = GetStaticQueryBody();
		bIncludeAuthToken = true; // app token, attached by descriptor scope
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
			TEXT("mutation RefreshAppToken {")
			TEXT("  refreshAppToken {")
			TEXT("    token")
			TEXT("    gameTokenId")
			TEXT("    appId")
			TEXT("    expiresAt")
			TEXT("    gameApiUrl")
			TEXT("    gameApiWsUrl")
			TEXT("    launchUrl")
			TEXT("  }")
			TEXT("}");
		return Body;
	}
};
