#pragma once
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FLoginRequest : ICrowdyQueryRequest
{
	
	FString Email;
	FString Password;
	
	virtual FName GetOperationName() const override
	{
		return "Login Request";
	}
	
	virtual void PrepareQuery() override
	{
		RuntimeVariables.Add(TEXT("email"), Email);
		RuntimeVariables.Add(TEXT("password"), Password);
		bIncludeAuthToken = false;
		bUseNestedJson = false;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override 
	{
		return EGraphQLQuery::Login;
	}
	
	virtual bool IsValid() const override
	{
		return RuntimeVariables.Num() == 2 && !Email.IsEmpty() && !Password.IsEmpty();
	}
};