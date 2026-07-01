#pragma once
#include "Queries/Authentication/FAuthResponseBase.h"

/** Response for the devLogin mutation (dev/test bypass; DEV_AUTH_BYPASS only). */
struct FDevLoginResponse : FAuthResponseBase
{
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::DevLogin;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Dev Login Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		ParseAuthPayload(JsonObject, TEXT("devLogin"));
	}
};
