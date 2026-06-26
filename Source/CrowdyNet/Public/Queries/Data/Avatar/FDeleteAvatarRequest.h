#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FDeleteAvatarRequest : ICrowdyQueryRequest
{
	int64 AvatarID;
	
	virtual FName GetOperationName() const override
	{
		return "Delete Avatar Request";
	}
	
	virtual void PrepareQuery() override
	{
		const FString AvatarIDStr = LexToString(AvatarID);
		RuntimeVariables.Add(TEXT("id"), AvatarIDStr);
		bIncludeAuthToken = true;
		bUseNestedJson = false;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::DeleteAvatar;
	}
	
	virtual bool IsValid() const override
	{
		return bIsValid;
	}
};