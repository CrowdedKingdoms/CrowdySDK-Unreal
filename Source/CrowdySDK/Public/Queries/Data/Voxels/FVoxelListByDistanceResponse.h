#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Shared/Types/Structures/Chunks/FChunkVoxelListBatch.h"
#include "Misc/Base64.h"

struct FVoxelListByDistanceResponse : ICrowdyQueryResponse
{
	TArray<FChunkVoxelListBatch> BatchVoxelListData;
	int64 CenterX, CenterY, CenterZ;
	int32 TotalUpdates = 0;
	int32 Skip = 0;
	int32 Limit = 0;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::ListVoxelUpdatesByDistance;
	}
	
	virtual FName GetOperationName() const override
	{
		return "Voxel List By Distance Response";
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("Invalid JSON Object");
			bIsValid = false;
			return;
		}

		const TSharedPtr<FJsonObject>* DataObject;
		const TSharedPtr<FJsonObject>* VoxelUpdatesByDistanceObject;

		// Extract data object first
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("Failed To extract data object from response.");
			bIsValid = false;
			return;
		}

		if (!(*DataObject)->TryGetObjectField(TEXT("listVoxelUpdatesByDistance"), VoxelUpdatesByDistanceObject))
		{
			ErrorMessage = TEXT("Failed to extract listVoxelUpdatesByDistance from response.");
			bIsValid = false;
			return;
		}

		// Extract chunks array from data
		const TArray<TSharedPtr<FJsonValue>>* ChunksArray;
		if (!(*VoxelUpdatesByDistanceObject)->TryGetArrayField(TEXT("chunks"), ChunksArray))
		{
			ErrorMessage = TEXT("Failed To extract chunks array from response.");
			bIsValid = false;
			return;
		}

		if (!(*VoxelUpdatesByDistanceObject)->TryGetNumberField(TEXT("limit"), Limit))
		{
			ErrorMessage = TEXT("Failed To extract limit from response.");
		}

		if (!(*VoxelUpdatesByDistanceObject)->TryGetNumberField(TEXT("skip"), Skip))
		{
			ErrorMessage = TEXT("Failed To extract limit from response.");
		}

		const TSharedPtr<FJsonObject>* CenterCoordinateObject;

		if (!(*VoxelUpdatesByDistanceObject)->TryGetObjectField(TEXT("centerCoordinate"), CenterCoordinateObject))
		{
			ErrorMessage = TEXT("Failed To extract centerCoordinate from response.");
			return;
		}

		CenterX = FCString::Atoi64(*(*CenterCoordinateObject)->GetStringField(TEXT("x")));
		CenterY = FCString::Atoi64(*(*CenterCoordinateObject)->GetStringField(TEXT("y")));
		CenterZ = FCString::Atoi64(*(*CenterCoordinateObject)->GetStringField(TEXT("z")));

		BatchVoxelListData.Reserve(200);

		TotalUpdates = ChunksArray->Num();

		// Process each chunk
		for (auto& ChunkData : *ChunksArray)
		{
			const TSharedPtr<FJsonObject>* ChunkObj;
			if (!ChunkData->TryGetObject(ChunkObj))
			{
				UE_LOG(LogTemp, Warning, TEXT("Failed to get chunk object"));
				continue;
			}

			// Extract chunk coordinates
			const TSharedPtr<FJsonObject>* ChunkCoordinates;
			if (!(*ChunkObj)->TryGetObjectField(TEXT("coordinates"), ChunkCoordinates))
			{
				UE_LOG(LogTemp, Warning, TEXT("Chunk coordinates not found"));
				continue;
			}

			FString sChunkX, sChunkY, sChunkZ;
			if (!(*ChunkCoordinates)->TryGetStringField(TEXT("x"), sChunkX) ||
				!(*ChunkCoordinates)->TryGetStringField(TEXT("y"), sChunkY) ||
				!(*ChunkCoordinates)->TryGetStringField(TEXT("z"), sChunkZ))
			{
				UE_LOG(LogTemp, Warning, TEXT("Failed to extract chunk coordinates"));
				continue;
			}

			int64 ChunkX = FCString::Atoi64(*sChunkX);
			int64 ChunkY = FCString::Atoi64(*sChunkY);
			int64 ChunkZ = FCString::Atoi64(*sChunkZ);

			// Extract voxels array for this chunk
			const TArray<TSharedPtr<FJsonValue>>* VoxelsArray;
			if (!(*ChunkObj)->TryGetArrayField(TEXT("voxels"), VoxelsArray))
			{
				UE_LOG(LogTemp, Warning, TEXT("Failed to extract voxels array for chunk %lld, %lld, %lld"),
				       ChunkX, ChunkY, ChunkZ);
				continue;
			}


			FChunkDataContainer DataContainer;

			// Process each voxel in the chunk
			for (auto& VoxelData : *VoxelsArray)
			{
				const TSharedPtr<FJsonObject>* VoxelObj;
				if (!VoxelData->TryGetObject(VoxelObj))
				{
					UE_LOG(LogTemp, Warning, TEXT("Failed to get voxel object"));
					continue;
				}

				// Extract voxel location
				const TSharedPtr<FJsonObject>* VoxelLocation;
				if (!(*VoxelObj)->TryGetObjectField(TEXT("location"), VoxelLocation))
				{
					UE_LOG(LogTemp, Warning, TEXT("Voxel location not found"));
					continue;
				}

				FString sVoxelX, sVoxelY, sVoxelZ, sVoxelType, sState;
				if (!(*VoxelLocation)->TryGetStringField(TEXT("x"), sVoxelX) ||
					!(*VoxelLocation)->TryGetStringField(TEXT("y"), sVoxelY) ||
					!(*VoxelLocation)->TryGetStringField(TEXT("z"), sVoxelZ) ||
					!(*VoxelObj)->TryGetStringField(TEXT("voxelType"), sVoxelType))
				{
					UE_LOG(LogTemp, Warning, TEXT("Failed to extract voxel data"));
					continue;
				}

				uint8 VoxelX = static_cast<uint8>(FCString::Atoi(*sVoxelX));
				uint8 VoxelY = static_cast<uint8>(FCString::Atoi(*sVoxelY));
				uint8 VoxelZ = static_cast<uint8>(FCString::Atoi(*sVoxelZ));
				uint8 VoxelType = static_cast<uint8>(FCString::Atoi(*sVoxelType));

				FVoxelState VoxelState; // default-initialized

				TArray<uint8> DecodedState;
				bool bStateFound = false;

				if ((*VoxelObj)->TryGetStringField(TEXT("state"), sState))
				{
					bStateFound = true;
				}

				if (bStateFound)
				{
					if (FBase64::Decode(sState, DecodedState))
					{
						if (DecodedState.Num() >= sizeof(FVoxelState))
						{
							VoxelState.DeserializeFromBytes(DecodedState);
						}
					}
				}

				FString TimestampString = (*VoxelObj)->GetStringField(TEXT("createdAt"));
				FDateTime Timestamp;

				if (!FDateTime::ParseIso8601(*TimestampString, Timestamp))
				{
					UE_LOG(LogTemp, Warning, TEXT("Failed to parse timestamp from response"));
				}

				// init coordinate and definition
				FVoxelCoordinate VoxelCoordinate(VoxelX, VoxelY, VoxelZ);

				FVoxelDefinition VoxelDefinition;
				VoxelDefinition.VoxelType = VoxelType;
				VoxelDefinition.VoxelState = VoxelState;
				VoxelDefinition.CreatedAt = Timestamp;


				// Add to data container for the new system
				DataContainer.VoxelStatesMap.Add(VoxelCoordinate, VoxelDefinition);
			}

			FChunkVoxelListBatch Batch;
			Batch.ChunkCoordinate.X = ChunkX;
			Batch.ChunkCoordinate.Y = ChunkY;
			Batch.ChunkCoordinate.Z = ChunkZ;
			Batch.VoxelListData = DataContainer;

			BatchVoxelListData.Add(MoveTemp(Batch));
		}
		
		bIsValid = true;
	}
};
