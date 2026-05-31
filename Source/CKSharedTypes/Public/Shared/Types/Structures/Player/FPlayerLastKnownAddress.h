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
	int64 ChunkX;

	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int64 ChunkY;

	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int64 ChunkZ;

	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int32 VoxelX;

	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int32 VoxelY;

	UPROPERTY(BlueprintReadWrite, Category = "Player Last Known Address")
	int32 VoxelZ;
};