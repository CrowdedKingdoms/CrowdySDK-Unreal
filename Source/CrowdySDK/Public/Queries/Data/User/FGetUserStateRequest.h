#pragma once
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FGetUserStateRequest : ICrowdyQueryRequest
{
	virtual FName GetOperationName() const override
	{
		return "Get User State Request";
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
		return EGraphQLQuery::GetUserState;
	}
	
	virtual bool IsValid() const override
	{
		return true;
	}
};