#pragma once

#include "CoreMinimal.h"
#include "Shared/Types/Structures/Voxels/FVoxelCoordinate.h"
#include "Shared/Types/Structures/Voxels/FVoxelDefinition.h"
#include "FChunkDataContainer.generated.h"

USTRUCT(BlueprintType)
struct FChunkDataContainer
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Version = 1;
	
	UPROPERTY()
	FInt64Vector ChunkCoordinate;

	UPROPERTY()
	TArray<uint8> VoxelData;
	
	UPROPERTY()
	TMap<FVoxelCoordinate, FVoxelDefinition> VoxelStatesMap;
	
	
	FChunkDataContainer() : ChunkCoordinate()
	{
		VoxelData.SetNum(16 * 16 * 16);
	}
};
