#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyVoxels.h"
#include "CrowdyVoxelsQueryActions.generated.h"

// One signature serves each node's success and failure pins, because a Blueprint async node builds its pins from its delegates.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyChunksFetchedAsyncResult, const TArray<FCrowdyVoxelChunk>&, Chunks, ECrowdyVoxelResult, Result, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyChunkFetchedAsyncResult, const FCrowdyVoxelChunk&, Chunk, ECrowdyVoxelResult, Result, const FString&, Message);

/** Fetches the platform's chunks around one chunk. C++ callers use UCrowdyVoxels::FetchChunksAround. */
UCLASS()
class CROWDYSERVICES_API UCrowdyFetchChunksAround : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** Every chunk the platform stores in range; a chunk it does not store is absent. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyChunksFetchedAsyncResult OnFetched;

	/** Nothing was fetched; Result says why. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyChunksFetchedAsyncResult OnFailed;

	/** Every chunk the platform stores within Range (1 to 8) of this chunk on every axis. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Fetch Chunks Around"), Category="Crowdy SDK|Voxels")
	static UCrowdyFetchChunksAround* FetchChunksAround(UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ, int32 Range = 2);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;
	int32 Range = 2;

	void Finish(const FCrowdyChunkFetch& Fetch);
};

/** Fetches one chunk. C++ callers use UCrowdyVoxels::FetchChunk. */
UCLASS()
class CROWDYSERVICES_API UCrowdyFetchChunk : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** The chunk; when the platform does not store it, it comes back empty with Stored false. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyChunkFetchedAsyncResult OnFetched;

	/** Nothing was fetched; Result says why. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyChunkFetchedAsyncResult OnFailed;

	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Fetch Chunk"), Category="Crowdy SDK|Voxels")
	static UCrowdyFetchChunk* FetchChunk(UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;

	void Finish(const FCrowdyChunkFetch& Fetch);
};
