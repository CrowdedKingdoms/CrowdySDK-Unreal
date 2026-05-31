#pragma once

#include "CoreMinimal.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"


struct FUpdateAvatarNameRequest : ICrowdyQueryRequest
{
	FString NewAvatarName;
	int64 AvatarID;
	
	virtual FName GetOperationName() const override
	{
		return "Update Avatar Name Request";
	}
	
	virtual void PrepareQuery() override
	{
		const FString AvatarIDStr = LexToString(AvatarID);
		RuntimeVariables.Add(TEXT("id"), AvatarIDStr);
		RuntimeVariables.Add(TEXT("name"), NewAvatarName);
		bIncludeAuthToken = true;
		bUseNestedJson = false;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::UpdateAvatarName;
	}
	
	virtual bool IsValid() const override
	{
		return !RuntimeVariables.IsEmpty() && RuntimeVariables.Num() == 2;
	}
};