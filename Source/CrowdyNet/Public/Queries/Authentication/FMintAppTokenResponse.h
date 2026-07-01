#pragma once
#include "Queries/Authentication/FAppTokenResponseBase.h"

/** Response for the mintAppToken mutation (session token -> app-scoped token). */
struct FMintAppTokenResponse : FAppTokenResponseBase
{
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::MintAppToken;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Mint App Token Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		ParseAppToken(JsonObject, TEXT("mintAppToken"));
	}
};
