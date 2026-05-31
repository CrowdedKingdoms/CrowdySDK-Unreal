#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Shared/Types/Structures/Chunks/FChunkDataContainer.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Misc/Base64.h"

struct FVoxelListResponse : ICrowdyQueryResponse
{
	int64 ChunkX, ChunkY, ChunkZ;
	FChunkDataContainer ChunkData;
	bool bDecrementCounter = true;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::VoxelList;
	}
	
	virtual FName GetOperationName() const override
	{
		return "Voxel List Response";
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("Invalid Json Object");
			bDecrementCounter = true;
			bIsValid = false;
			return;
		}

		const TSharedPtr<FJsonObject>* DataObject;
		const TSharedPtr<FJsonObject>* VoxelListObject;

		// Extract data object first
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObject) || !DataObject->IsValid())
		{
			ErrorMessage = TEXT("Failed to extract data object from response");
			bDecrementCounter = true;
			bIsValid = false;
			return;
		}

		if (!(*DataObject)->TryGetObjectField(TEXT("getVoxelList"), VoxelListObject))
		{
			ErrorMessage = TEXT("Failed to extract getVoxelList from response");
			bDecrementCounter = true;
			bIsValid = false;
			return;
		}
		
		if (!USerializationFunctionLibrary::ExtractChunkCoordinates(*VoxelListObject, ChunkX, ChunkY, ChunkZ))
		{
			ErrorMessage = TEXT("Failed to extract chunk coordinates from json response");
			bDecrementCounter = true;
			bIsValid = false;
			return;
		}

		// Extract voxel list from data
		const TArray<TSharedPtr<FJsonValue>>* VoxelListArray;

		if (!(*VoxelListObject)->TryGetArrayField(TEXT("voxels"), VoxelListArray))
		{
			ErrorMessage = TEXT("Failed to extract voxel list from response");
			bDecrementCounter = true;
			bIsValid = false;
			return;
		}
		
		for (auto& VoxelData : *VoxelListArray)
		{
			const TSharedPtr<FJsonObject>* VoxelStateObj;
			VoxelData->TryGetObject(VoxelStateObj);

			const TSharedPtr<FJsonObject>* Coordinates;
			if (!(*VoxelStateObj)->TryGetObjectField(TEXT("location"), Coordinates))
			{
				UE_LOG(LogTemp, Warning, TEXT("Coordinates not found in voxel list element object"));
				continue;
			}

			FString sX, sY, sZ, sType, sState;
			if (!(*Coordinates)->TryGetStringField(TEXT("x"), sX) ||
				!(*Coordinates)->TryGetStringField(TEXT("y"), sY) ||
				!(*Coordinates)->TryGetStringField(TEXT("z"), sZ) ||
				!(*VoxelStateObj)->TryGetStringField(TEXT("voxelType"), sType))
			{
				UE_LOG(LogTemp, Warning, TEXT("Failed to extract core voxel list item(s)"));
			}

			const uint8 cx = static_cast<uint8>(FCString::Atoi(*sX));
			const uint8 cy = static_cast<uint8>(FCString::Atoi(*sY));
			const uint8 cz = static_cast<uint8>(FCString::Atoi(*sZ));
			const uint8 VoxelType = static_cast<uint8>(FCString::Atoi(*sType));

			FVoxelState VoxelState; // default-initialized

			TArray<uint8> DecodedState;

			bool bStateFound = false;

			if ((*VoxelStateObj)->TryGetStringField(TEXT("state"), sState))
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

			ChunkData.VoxelStatesMap.Add(FVoxelCoordinate(cx, cy, cz), FVoxelDefinition(1, VoxelType, VoxelState));
		}
		
		bDecrementCounter = true;
		bIsValid = true;
	}
};
