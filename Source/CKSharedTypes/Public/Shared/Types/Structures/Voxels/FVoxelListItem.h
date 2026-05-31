#pragma once

#include "CoreMinimal.h"
#include "FVoxelListItem.generated.h"

USTRUCT(BlueprintType, Blueprintable)
struct FVoxelListItem
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VoxelListItem")
    uint8 Version = 1;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VoxelListItem")
    uint8 x;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VoxelListItem")
    uint8 y;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VoxelListItem")
    uint8 z;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VoxelListItem")
    uint8 type;

    // Default constructor (needed for Unreal Engine)
    FVoxelListItem()
        : x(0), y(0), z(0), type(0)  // Initialize default values
    {}
};