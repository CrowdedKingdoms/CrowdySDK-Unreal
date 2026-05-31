#pragma once
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FTeleportRequest : ICrowdyQueryRequest
{
	FString UUID;
	int64 MapID;
	int64 ChunkX;
	int64 ChunkY;
	int64 ChunkZ;
	
	int32 VoxelX;
	int32 VoxelY;
	int32 VoxelZ;
	
	virtual FName GetOperationName() const override
	{
		return "Teleport Request";
	}
	
	// TODO: Verify this format and apply corrections later if any
	virtual void PrepareQuery() override
	{
		RuntimeVariables.Add(TEXT("appId"), FString::Printf(TEXT("%lld"), MapID));
		RuntimeVariables.Add(TEXT("cx"), FString::Printf(TEXT("%lld"), ChunkX));
		RuntimeVariables.Add(TEXT("cy"), FString::Printf(TEXT("%lld"), ChunkY));
		RuntimeVariables.Add(TEXT("cz"), FString::Printf(TEXT("%lld"), ChunkZ));
		RuntimeVariables.Add(TEXT("vx"), FString::Printf(TEXT("%d"), VoxelX));
		RuntimeVariables.Add(TEXT("vy"), FString::Printf(TEXT("%d"), VoxelY));
		RuntimeVariables.Add(TEXT("vz"), FString::Printf(TEXT("%d"), VoxelZ));
		RuntimeVariables.Add(TEXT("uuid"), UUID);
		
		bIncludeAuthToken = true;
		bUseNestedJson = true;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::TeleportRequest;
	}
	
	virtual bool IsValid() const override
	{
		return MapID > -1 && !UUID.IsEmpty() && UUID.Len() == 32;
	}
};