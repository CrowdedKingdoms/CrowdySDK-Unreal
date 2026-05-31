#pragma once

UENUM(BlueprintType)
enum class EVoxelSystemType : uint8
{
	Classic UMETA(DisplayName = "Classic"),
	VoxelPluginLegacy UMETA(DisplayName = "Voxel Plugin Legacy"),
	Disabled UMETA(DisplayName = "Disabled")
};