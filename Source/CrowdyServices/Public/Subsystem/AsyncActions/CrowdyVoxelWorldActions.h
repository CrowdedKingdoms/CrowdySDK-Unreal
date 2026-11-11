#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystem/CrowdyVoxels.h"
#include "CrowdyVoxelWorldActions.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyVoxelReloadAsyncResult, ECrowdyVoxelResult, Result, const FString&, Message);

/** Fetches a loaded chunk of the voxel world again. C++ callers use UCrowdyVoxelWorld::ReloadChunk. */
UCLASS()
class CROWDYSERVICES_API UCrowdyReloadChunk : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/** A fresh fetch has merged; On Chunk Changed has already named every cell it changed. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyVoxelReloadAsyncResult OnDone;

	/** The chunk is not loaded, the connection dropped, the fetch failed, or none completed within 30 seconds. */
	UPROPERTY(BlueprintAssignable)
	FCrowdyVoxelReloadAsyncResult OnFailed;

	/** Neither pin fires once the world ends. */
	UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Reload Chunk"), Category="Crowdy SDK|Voxels")
	static UCrowdyReloadChunk* ReloadChunk(UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	TWeakObjectPtr<UWorld> ReloadWorld;
	FDelegateHandle WorldCleanupHandle;
	FInt64Vector Chunk = FInt64Vector::ZeroValue;
	bool bEnded = false;

	void Finish(ECrowdyVoxelResult Result, const FString& Message);
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void End();
};
