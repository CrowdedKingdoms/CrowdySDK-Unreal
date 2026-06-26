#pragma once
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FUDPAddressRequest : ICrowdyQueryRequest
{
	virtual FName GetOperationName() const override
	{
		return "UDP Address Request";
	}
	
	virtual void PrepareQuery() override
	{
		RuntimeVariables.Empty();
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::UDP_Access;
	}
	
	virtual bool IsValid() const override
	{
		return true;
	}
};