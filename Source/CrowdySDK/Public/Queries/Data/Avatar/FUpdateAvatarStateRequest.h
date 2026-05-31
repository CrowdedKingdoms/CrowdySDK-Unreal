#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FUpdateAvatarStateRequest : ICrowdyQueryRequest
{
	FString NewAvatarState;
	int64 AvatarID;
	
	virtual FName GetOperationName() const override
	{
		return "Update Avatar State";
	}
	
	virtual void PrepareQuery() override
	{
		const FString AvatarIDStr = LexToString(AvatarID);
		RuntimeVariables.Add(TEXT("id"), AvatarIDStr);
		RuntimeVariables.Add("publicState", NewAvatarState);
		bIncludeAuthToken = true;
		bUseNestedJson = false;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::UpdateAvatarState;
	}
	
	virtual bool IsValid() const override
	{
		return bIsValid;
	}
};