#pragma once

#include "CoreMinimal.h"
#include "FChunkData.generated.h"

USTRUCT(BlueprintType, Blueprintable)
struct FChunkData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category="Voxel")
	uint8 Version = 1;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Voxel")
	int64 ChunkX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Voxel")
	int64 ChunkY;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Voxel")
	int64 ChunkZ;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Voxel")
	TArray<uint8> VoxelData;


	FChunkData()
		:ChunkX(0),ChunkY(0), ChunkZ(0) {}
};