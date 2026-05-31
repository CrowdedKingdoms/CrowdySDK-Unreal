#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"
#include "FChunkMeshBuildResult.generated.h"

USTRUCT()
struct FChunkMeshBuildResult
{
	GENERATED_BODY()
	TArray<FVector> Vertices;
	TArray<int32>   Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> VertexColors;
	TArray<FProcMeshTangent> Tangents;

	struct FSection
	{
		int64 AtlasOverride;
		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FLinearColor> VertexColors;
		TArray<FProcMeshTangent> Tangents;
	};
	TArray<FSection> OverrideSections;

	TMap<uint8, TArray<FTransform>> VLOTransforms;

	TArray<FPlacedObjectState> PlacedObjects;
};
