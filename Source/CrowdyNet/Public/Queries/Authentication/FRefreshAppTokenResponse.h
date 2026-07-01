#pragma once
#include "Queries/Authentication/FAppTokenResponseBase.h"

/** Response for the refreshAppToken mutation (same-app token rotation). */
struct FRefreshAppTokenResponse : FAppTokenResponseBase
{
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::RefreshAppToken;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Refresh App Token Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		ParseAppToken(JsonObject, TEXT("refreshAppToken"));
	}
};
