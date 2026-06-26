#pragma once

#include "CoreMinimal.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FCreateAvatarRequest : ICrowdyQueryRequest
{
	FString AvatarName;
	
	virtual FName GetOperationName() const override
	{
		return "Create Avatar Request";
	}
	
	virtual void PrepareQuery() override
	{
		RuntimeVariables.Add(TEXT("name"), AvatarName);
		bIncludeAuthToken = true;
		bUseNestedJson = false;
		bIsValid = true;
	}
	
	virtual  EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::CreateAvatar;
	}
	
	virtual bool IsValid() const override
	{
		return !AvatarName.IsEmpty();
	}
	
};