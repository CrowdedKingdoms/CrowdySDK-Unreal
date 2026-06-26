#pragma once
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"

struct FUpdateChunkRequest : ICrowdyQueryRequest
{
	TArray<uint8> Voxels;
	int64 AppID;
	int64 ChunkX;
	int64 ChunkY;
	int64 ChunkZ;
	
	virtual FName GetOperationName() const override
	{
		return "Update Chunk Request";
	}
	
	virtual void PrepareQuery() override
	{
		RuntimeVariables.Add(TEXT("appId"), FString::Printf(TEXT("%lld"), AppID));
		RuntimeVariables.Add(TEXT("x"), FString::Printf(TEXT("%lld"), ChunkX));
		RuntimeVariables.Add(TEXT("y"), FString::Printf(TEXT("%lld"), ChunkY));
		RuntimeVariables.Add(TEXT("z"), FString::Printf(TEXT("%lld"), ChunkZ));

		const FString EncodedVoxels = FBase64::Encode(Voxels);
		RuntimeVariables.Add(TEXT("voxels"), EncodedVoxels);
		bIncludeAuthToken = true;
		bUseNestedJson = false;
		bIsValid = true;
	}
	
	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::UpdateChunk;
	}
	
	virtual bool IsValid() const override
	{
		return Voxels.Num() > 0 && AppID > -1 && RuntimeVariables.Num() == 5;
	}
	
	
};