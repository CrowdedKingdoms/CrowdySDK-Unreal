#pragma once
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Shared/Types/Structures/Chunks/FChunkDataContainer.h"

struct FGetChunkResponse : ICrowdyQueryResponse
{
	TArray<FChunkDataContainer> ChunkData;
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::GetChunkByDistance;
	}
	
	virtual FName GetOperationName() const override
	{
		return TEXT("Get Chunk Response");
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		
	}
	
};
