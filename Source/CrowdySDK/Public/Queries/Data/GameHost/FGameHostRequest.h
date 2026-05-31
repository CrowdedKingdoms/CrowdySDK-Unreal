#pragma once
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FGameHostRequest : ICrowdyQueryRequest
{
	int64 AppId = 0;

	virtual FName GetOperationName() const override
	{
		return TEXT("Game Host Request");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::GameHost;
	}

	virtual bool IsValid() const override
	{
		return bIsValid;
	}

	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		RuntimeVariables.Add(TEXT("appId"), FString::Printf(TEXT("%lld"), AppId));
		bIncludeAuthToken = true;
		bUseNestedJson    = false;
		bIsValid          = true;
	}
};
