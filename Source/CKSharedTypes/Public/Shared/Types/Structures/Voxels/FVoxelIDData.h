// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Shared/Types/Enums/Voxels/EVoxelType.h"
#include "FVoxelState.h"
#include "Shared/Types/Enums/Voxels/EVoxelSystemType.h"
#include "FVoxelIDData.generated.h"

USTRUCT(BlueprintType, Blueprintable)
struct FVoxelIDData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category="Voxel IDData")
	int64 Cx;

	UPROPERTY(BlueprintReadWrite, Category="Voxel IDData")
	int64 Cy;

	UPROPERTY(BlueprintReadWrite, Category="Voxel IDData")
	int64 Cz;
    
	UPROPERTY(BlueprintReadWrite, Category="Voxel IDData")
	int32 Vx;

	UPROPERTY(BlueprintReadWrite, Category="Voxel IDData")
	int32 Vy;

	UPROPERTY(BlueprintReadWrite, Category="Voxel IDData")
	int32 Vz;

	UPROPERTY(BlueprintReadWrite, Category="Voxel IDData")
	EVoxelType VoxelType;

	UPROPERTY(BlueprintReadWrite, Category="Voxel IDData")
	FVoxelState VoxelState;

	UPROPERTY(BlueprintReadWrite, Category="Voxel IDData")
	EVoxelSystemType VoxelSystemType; 
};