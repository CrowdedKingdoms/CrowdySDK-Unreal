#pragma once
#include "CoreMinimal.h"
#include "CKSharedTypes/Public/Shared/Types/Structures/Voxels/FVoxelState.h"
#include "FVoxelDefinition.generated.h"

USTRUCT()
struct FVoxelDefinition
{
	GENERATED_BODY()
	UPROPERTY()
	uint8 Version = 1;

	UPROPERTY()
	uint8 VoxelType = 0;

	UPROPERTY()
	FVoxelState VoxelState;
	
	UPROPERTY()
	FDateTime CreatedAt = FDateTime::MinValue();
};