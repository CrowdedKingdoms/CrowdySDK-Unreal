#pragma once
#include "Queries/Authentication/FAuthResponseBase.h"

/** Response for the completeLoginLink mutation (magic-link sign-in step 2). */
struct FCompleteLoginLinkResponse : FAuthResponseBase
{
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::CompleteLoginLink;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Complete Login Link Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		ParseAuthPayload(JsonObject, TEXT("completeLoginLink"));
	}
};
