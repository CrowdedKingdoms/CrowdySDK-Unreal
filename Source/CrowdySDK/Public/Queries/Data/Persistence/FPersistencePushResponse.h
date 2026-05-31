#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * Response from the UpdateChunk mutation when issued by
 * UCrowdyPersistenceSubsystem::PushState.
 *
 * On success, echoes back the chunk coordinates that were written.
 * The subsystem logs the result and can surface it to callers.
 */
struct FPersistencePushResponse : ICrowdyQueryResponse
{
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::PersistencePush;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Persistence Push Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("Invalid JSON object.");
			return;
		}

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj) || !DataObj->IsValid())
		{
			ErrorMessage = TEXT("Missing 'data' field.");
			return;
		}

		const TSharedPtr<FJsonObject>* UpdateChunkObj;
		if (!(*DataObj)->TryGetObjectField(TEXT("updateChunk"), UpdateChunkObj) || !UpdateChunkObj->IsValid())
		{
			ErrorMessage = TEXT("Missing 'updateChunk' field.");
			return;
		}

		if (!USerializationFunctionLibrary::ExtractChunkCoordinates(*UpdateChunkObj, ChunkX, ChunkY, ChunkZ))
		{
			ErrorMessage = TEXT("Failed to extract chunk coordinates.");
			return;
		}

		bIsValid = true;
	}
};
