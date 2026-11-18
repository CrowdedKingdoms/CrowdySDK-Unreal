#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Subsystem/CrowdyVoxels.h"
#include "CrowdyVoxelLibrary.generated.h"

/** Placing and reading voxels. Usable with no game instance; the chunk size is then 1,600. */
UCLASS()
class CROWDYSERVICES_API UCrowdyVoxelLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** The chunk's minimum corner in the world. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels", meta=(WorldContext="WorldContextObject"))
	static FVector GetChunkOrigin(const UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ);

	/** One cell's edge in world units: the game session's chunk size over 16, 100 by default. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels", meta=(WorldContext="WorldContextObject"))
	static double GetVoxelSize(const UObject* WorldContextObject);

	/** The chunk a world location is in, and the cell (each axis 0 to 15) it falls in. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels", meta=(WorldContext="WorldContextObject"))
	static void WorldToVoxel(const UObject* WorldContextObject, FVector Location, int64& ChunkX, int64& ChunkY, int64& ChunkZ, FIntVector& Position);

	/** The centre of a cell in the world. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels", meta=(WorldContext="WorldContextObject"))
	static FVector VoxelToWorld(const UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ, FIntVector Position);

	/** The voxel at a position: its edit when it has one, else the grid's type; Type 0 when the chunk holds nothing there. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels")
	static FCrowdyVoxel GetVoxel(const FCrowdyVoxelChunk& Chunk, FIntVector Position);

	/** Every voxel whose type is not 0, with its state: the grid's cells in index order, then edits the grid cannot hold. */
	UFUNCTION(BlueprintPure, Category="Crowdy SDK|Voxels")
	static TArray<FCrowdyVoxel> GetFilledVoxels(const FCrowdyVoxelChunk& Chunk);
};
