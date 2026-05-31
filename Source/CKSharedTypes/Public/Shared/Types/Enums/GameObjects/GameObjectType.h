#pragma once

#include "CoreMinimal.h"

UENUM(BlueprintType)
enum class EGameObjectType : uint8
{
	None		UMETA(DisplayName = "Default"),
	Voxel		UMETA(DisplayName = "Voxel"),
	VLO			UMETA(DisplayName = "Voxel Like Object"),
	StaticMesh	UMETA(DisplayName = "Static Mesh"),
	ActorBP		UMETA(DisplayName = "Actor Blueprint")
};
