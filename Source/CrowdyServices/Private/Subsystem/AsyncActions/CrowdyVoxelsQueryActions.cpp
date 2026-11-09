#include "Subsystem/AsyncActions/CrowdyVoxelsQueryActions.h"
#include "CrowdyServicesActionSupport.h"

namespace
{
	FCrowdyChunkFetch VoxelsMissingSubsystem()
	{
		FCrowdyChunkFetch Missing;
		Missing.Message = TEXT("UCrowdyVoxels subsystem not found");
		return Missing;
	}
}

UCrowdyFetchChunksAround* UCrowdyFetchChunksAround::FetchChunksAround(UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ, int32 Range)
{
	UCrowdyFetchChunksAround* Action = NewObject<UCrowdyFetchChunksAround>();
	Action->WorldContextObject = WorldContextObject;
	Action->ChunkX = ChunkX;
	Action->ChunkY = ChunkY;
	Action->ChunkZ = ChunkZ;
	Action->Range = Range;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyFetchChunksAround::Activate()
{
	UCrowdyVoxels* Voxels = GetServiceSubsystem<UCrowdyVoxels>(WorldContextObject);
	if (!Voxels)
	{
		Finish(VoxelsMissingSubsystem());
		return;
	}
	TWeakObjectPtr<UCrowdyFetchChunksAround> WeakThis(this);
	Voxels->FetchChunksAround(ChunkX, ChunkY, ChunkZ, Range, [WeakThis](const FCrowdyChunkFetch& Fetch)
	{
		if (UCrowdyFetchChunksAround* Self = WeakThis.Get())
		{
			Self->Finish(Fetch);
		}
	});
}

void UCrowdyFetchChunksAround::Finish(const FCrowdyChunkFetch& Fetch)
{
	FCrowdyChunksFetchedAsyncResult& Pin = Fetch.Result == ECrowdyVoxelResult::Ok ? OnFetched : OnFailed;
	Pin.Broadcast(Fetch.Chunks, Fetch.Result, Fetch.Message);
	SetReadyToDestroy();
}

UCrowdyFetchChunk* UCrowdyFetchChunk::FetchChunk(UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ)
{
	UCrowdyFetchChunk* Action = NewObject<UCrowdyFetchChunk>();
	Action->WorldContextObject = WorldContextObject;
	Action->ChunkX = ChunkX;
	Action->ChunkY = ChunkY;
	Action->ChunkZ = ChunkZ;
	Action->RegisterWithGameInstance(WorldContextObject);
	return Action;
}

void UCrowdyFetchChunk::Activate()
{
	UCrowdyVoxels* Voxels = GetServiceSubsystem<UCrowdyVoxels>(WorldContextObject);
	if (!Voxels)
	{
		Finish(VoxelsMissingSubsystem());
		return;
	}
	TWeakObjectPtr<UCrowdyFetchChunk> WeakThis(this);
	Voxels->FetchChunk(ChunkX, ChunkY, ChunkZ, [WeakThis](const FCrowdyChunkFetch& Fetch)
	{
		if (UCrowdyFetchChunk* Self = WeakThis.Get())
		{
			Self->Finish(Fetch);
		}
	});
}

void UCrowdyFetchChunk::Finish(const FCrowdyChunkFetch& Fetch)
{
	const bool bOk = Fetch.Result == ECrowdyVoxelResult::Ok && Fetch.Chunks.Num() == 1;
	FCrowdyChunkFetchedAsyncResult& Pin = bOk ? OnFetched : OnFailed;
	Pin.Broadcast(bOk ? Fetch.Chunks[0] : FCrowdyVoxelChunk(), Fetch.Result, Fetch.Message);
	SetReadyToDestroy();
}
