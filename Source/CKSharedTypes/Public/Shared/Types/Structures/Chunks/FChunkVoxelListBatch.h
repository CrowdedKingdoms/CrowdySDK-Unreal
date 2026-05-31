#pragma once
#include "FChunkDataContainer.h"
#include "FChunkVoxelListBatch.generated.h"

USTRUCT()
struct FChunkVoxelListBatch
{
	GENERATED_BODY()

	FInt64Vector ChunkCoordinate;

	FChunkDataContainer VoxelListData;
};
