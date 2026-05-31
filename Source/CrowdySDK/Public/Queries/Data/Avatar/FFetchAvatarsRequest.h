#pragma once

#include "CoreMinimal.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FFetchAvatarsRequest : ICrowdyQueryRequest
{
	virtual FName GetOperationName() const override
	{
		return "Fetch Avatar Request";
	}
	
	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		bIncludeAuthToken = true;
		bUseNestedJson = false;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::MyAvatars;
	}
	
	virtual bool IsValid() const override
	{
		return bIncludeAuthToken;
	}
};