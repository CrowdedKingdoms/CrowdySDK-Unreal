#pragma once

#include "CoreMinimal.h"
#include "Subsystem/CrowdyVoxelWorld.h"
#include "UObject/Object.h"
#include "CrowdyVoxelWorldTestTypes.generated.h"

/** Records what the voxel world broadcasts, in order, for its automation tests. */
UCLASS(Transient)
class UCrowdyVoxelWorldRecorder : public UObject
{
	GENERATED_BODY()

public:
	TArray<FString> Log;
	TArray<FInt64Vector> Loaded;
	TArray<FInt64Vector> Unloaded;
	TArray<FIntVector> ChangedPositions;
	TMap<int64, ECrowdyVoxelResult> Results;

	UFUNCTION()
	void HandleLoaded(int64 ChunkX, int64 ChunkY, int64 ChunkZ)
	{
		Loaded.Add(FInt64Vector(ChunkX, ChunkY, ChunkZ));
	}

	UFUNCTION()
	void HandleUnloaded(int64 ChunkX, int64 ChunkY, int64 ChunkZ)
	{
		Unloaded.Add(FInt64Vector(ChunkX, ChunkY, ChunkZ));
	}

	UFUNCTION()
	void HandleChanged(int64 ChunkX, int64 ChunkY, int64 ChunkZ, const TArray<FIntVector>& Positions)
	{
		ChangedPositions.Append(Positions);
		for (const FIntVector& Position : Positions)
		{
			Log.Add(FString::Printf(TEXT("changed %d %d %d"), Position.X, Position.Y, Position.Z));
		}
	}

	UFUNCTION()
	void HandleDone(int64 EditId, const FCrowdyVoxelWrite& Write, ECrowdyVoxelResult Result, const FString& Message)
	{
		Results.Add(EditId, Result);
		Log.Add(FString::Printf(TEXT("done %lld %s"), EditId, *UEnum::GetValueAsString(Result)));
	}
};
