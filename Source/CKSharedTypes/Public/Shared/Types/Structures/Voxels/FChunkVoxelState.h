#pragma once

#include "CoreMinimal.h"
#include "Shared/Types/Structures/Voxels/FVoxelState.h"
#include "FChunkVoxelState.generated.h"

USTRUCT(Blueprintable, BlueprintType)
struct FChunkVoxelState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Chunk Voxel State")
	uint8 Version;
	
	UPROPERTY(BlueprintReadWrite, Category = "Chunk Voxel State")
	uint8 Vx;

	UPROPERTY(BlueprintReadWrite, Category = "Chunk Voxel State")
	uint8 Vy;

	UPROPERTY(BlueprintReadWrite, Category = "Chunk Voxel State")
	uint8 Vz;

	UPROPERTY(BlueprintReadWrite, Category = "Chunk Voxel State")
	uint8 VoxelType;
	
	UPROPERTY()
	uint16 StateLength;

	UPROPERTY(BlueprintReadWrite, Category = "Chunk Voxel State")
	FVoxelState VoxelState;

	FChunkVoxelState()
		: Version(1), Vx(0), Vy(0), Vz(0), VoxelType(0), StateLength(0), VoxelState()
	{
	}
};