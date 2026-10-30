#pragma once
#include "CoreMinimal.h"
#include "FPlayerLastKnownAddress.generated.h"

USTRUCT(BlueprintType)
struct FPlayerLastKnownAddress
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Version = 1;
	
	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int64 ChunkX = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int64 ChunkY = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int64 ChunkZ = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int32 VoxelX = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int32 VoxelY = 0;

	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int32 VoxelZ = 0;
};