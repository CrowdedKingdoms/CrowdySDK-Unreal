#pragma once
#include "Queries/Authentication/FAuthResponseBase.h"

/**
 * Response for the socialLoginComplete mutation (social sign-in step 2). Returns the
 * same AuthResponse payload as password / dev / magic-link sign-in, so it reuses
 * FAuthResponseBase and feeds the shared mint pipeline.
 */
struct FSocialLoginCompleteResponse : FAuthResponseBase
{
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::SocialLoginComplete;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Social Login Complete Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		ParseAuthPayload(JsonObject, TEXT("socialLoginComplete"));
	}
};
