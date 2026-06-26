#pragma once
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FVoxelListRequest : ICrowdyQueryRequest
{
	int64 ChunkX;
	int64 ChunkY;
	int64 ChunkZ;
	int64 AppID = -1; 
	
	virtual FName GetOperationName() const override
	{
		return "Voxel List Request";
	}
	
	virtual void PrepareQuery() override
	{
		RuntimeVariables.Add(TEXT("appId"), FString::Printf(TEXT("%lld"), AppID));
		RuntimeVariables.Add(TEXT("x"), FString::Printf(TEXT("%lld"), ChunkX));
		RuntimeVariables.Add(TEXT("y"), FString::Printf(TEXT("%lld"), ChunkY));
		RuntimeVariables.Add(TEXT("z"), FString::Printf(TEXT("%lld"), ChunkZ));
		bIncludeAuthToken = true;
		bUseNestedJson = false;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::VoxelList;
	}
	
	virtual bool IsValid() const override
	{
		return AppID > -1 && RuntimeVariables.Num() == 4;
	}
};