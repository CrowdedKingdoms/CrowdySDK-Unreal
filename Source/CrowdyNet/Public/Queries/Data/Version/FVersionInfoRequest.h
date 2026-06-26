#pragma once
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FVersionInfoRequest : ICrowdyQueryRequest
{
	virtual FName GetOperationName() const override
	{
		return "Version Info Request";
	}
	
	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		bIncludeAuthToken = false;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::GetVersionInfo;
	}
	
	virtual bool IsValid() const override
	{
		return true;
	}
};