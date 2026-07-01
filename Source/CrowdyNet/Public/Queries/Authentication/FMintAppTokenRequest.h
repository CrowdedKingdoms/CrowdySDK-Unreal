#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"

/**
 * Mint an app-scoped gameplay token from the identity SESSION token (Bearer =
 * session token). appId is a BigInt emitted as a JSON string. Uses an inline
 * query body. The descriptor routes this to the Management endpoint with the
 * SESSION token (see FCrowdyQueryDescriptor.cpp).
 */
struct FMintAppTokenRequest : ICrowdyQueryRequest
{
	int64 AppID = 0;

	virtual FName GetOperationName() const override
	{
		return TEXT("Mint App Token");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::MintAppToken;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		// appId is BigInt -> sent as a JSON string (flat string var, no nesting).
		RuntimeVariables.Add(TEXT("appId"), FString::Printf(TEXT("%lld"), AppID));

		InlineQueryBody  = GetStaticQueryBody();
		bIncludeAuthToken = true; // session token, attached by descriptor scope
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return AppID > 0;
	}

private:
	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("mutation MintAppToken($appId: BigInt!) {")
			TEXT("  mintAppToken(input: { appId: $appId }) {")
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
