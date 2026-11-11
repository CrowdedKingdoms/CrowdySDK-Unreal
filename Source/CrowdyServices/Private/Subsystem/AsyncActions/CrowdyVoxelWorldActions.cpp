#include "Subsystem/AsyncActions/CrowdyVoxelWorldActions.h"
#include "Engine/World.h"
#include "Subsystem/CrowdyVoxelWorld.h"

UCrowdyReloadChunk* UCrowdyReloadChunk::ReloadChunk(UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ)
{
	UCrowdyReloadChunk* Action = NewObject<UCrowdyReloadChunk>();
	Action->WorldContextObject = WorldContextObject;
	Action->Chunk = FInt64Vector(ChunkX, ChunkY, ChunkZ);
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyReloadChunk::Activate()
{
	UWorld* World = WorldContextObject.IsValid() ? WorldContextObject->GetWorld() : nullptr;
	UCrowdyVoxelWorld* Voxels = World ? World->GetSubsystem<UCrowdyVoxelWorld>() : nullptr;
	if (!Voxels)
	{
		Finish(ECrowdyVoxelResult::Failed, TEXT("UCrowdyVoxelWorld subsystem not found"));
		return;
	}
	// The voxel world drops a waiting reload when its world ends, so the node ends with it, firing neither pin.
	ReloadWorld = World;
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &UCrowdyReloadChunk::HandleWorldCleanup);
	TWeakObjectPtr<UCrowdyReloadChunk> WeakThis(this);
	Voxels->ReloadChunk(Chunk.X, Chunk.Y, Chunk.Z, [WeakThis](ECrowdyVoxelResult Result, const FString& Message)
	{
		if (UCrowdyReloadChunk* Self = WeakThis.Get())
		{
			Self->Finish(Result, Message);
		}
	});
}

void UCrowdyReloadChunk::Finish(ECrowdyVoxelResult Result, const FString& Message)
{
	if (bEnded)
	{
		return;
	}
	FCrowdyVoxelReloadAsyncResult& Pin = Result == ECrowdyVoxelResult::Ok ? OnDone : OnFailed;
	Pin.Broadcast(Result, Message);
	End();
}

void UCrowdyReloadChunk::HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	if (World == ReloadWorld.Get())
	{
		End();
	}
}

void UCrowdyReloadChunk::End()
{
	if (bEnded)
	{
		return;
	}
	bEnded = true;
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	SetReadyToDestroy();
}
