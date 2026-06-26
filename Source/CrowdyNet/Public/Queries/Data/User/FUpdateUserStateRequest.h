#pragma once
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FUpdateUserStateRequest : ICrowdyQueryRequest
{
	// Serialize and convert data to UTF-8 Format (Encode as Base64) and then set this string to that
	FString EncodedStringState;
	
	virtual FName GetOperationName() const override
	{
		return "Update User State Request";
	}
	
	virtual void PrepareQuery() override
	{
		RuntimeVariables.Add(TEXT("stateStr"), EncodedStringState);
		bIncludeAuthToken = true;
		bUseNestedJson = false;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::UpdateUserState;
	}
	
	virtual bool IsValid() const override
	{
		return !EncodedStringState.IsEmpty() && RuntimeVariables.Num() == 1;
	}
	
};