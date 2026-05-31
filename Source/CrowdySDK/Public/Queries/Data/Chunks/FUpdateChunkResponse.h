#pragma once
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Utils/SerializationFunctionLibrary.h"

struct FUpdateChunkResponse : ICrowdyQueryResponse
{
	int64 ChunkX;
	int64 ChunkY;
	int64 ChunkZ;
	
	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::UpdateChunk;
	}
	
	virtual FName GetOperationName() const override
	{
		return TEXT("Update Chunk Response");
	}
	
	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("Invalid Json Object.");
			bIsValid = false;
			return;
		}

		const TSharedPtr<FJsonObject>* DataObject;
		const TSharedPtr<FJsonObject>* UpdateChunkObject;

		// Extract data object first
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract data object from response.");
			bIsValid = false;
			return;
		}

		// Extract updateChunk object from data
		if (!(*DataObject)->TryGetObjectField(TEXT("updateChunk"), UpdateChunkObject) || !UpdateChunkObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract updateChunk object from response.");
			bIsValid = false;
			return;
		}

		// Extract coordinates from the updateChunk object
		if (!USerializationFunctionLibrary::ExtractChunkCoordinates(*UpdateChunkObject, ChunkX, ChunkY, ChunkZ))
		{
			ErrorMessage = TEXT("Failed to extract chunk coordinates from response json.");
			bIsValid = false;
			return;
		}
		
	}
};
