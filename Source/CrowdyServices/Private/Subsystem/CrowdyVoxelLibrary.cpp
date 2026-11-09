#include "Subsystem/CrowdyVoxelLibrary.h"
#include "Algo/BinarySearch.h"
#include "CrowdyVoxelDecode.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Subsystem/CrowdyGameSession.h"

using namespace CrowdyVoxelDecode;

namespace
{
	constexpr double DefaultChunkSize = 1600.0;

	double ChunkSizeFor(const UObject* WorldContextObject)
	{
		const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
		const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
		const UCrowdyGameSession* Session = GameInstance ? GameInstance->GetSubsystem<UCrowdyGameSession>() : nullptr;
		return Session && Session->GetChunkSize() > 0.0 ? Session->GetChunkSize() : DefaultChunkSize;
	}

	// The whole chunk index along one axis, held inside int64 for any finite location.
	int64 ChunkAlong(double Location, double ChunkSize)
	{
		const double Chunk = FMath::FloorToDouble(Location / ChunkSize);
		return static_cast<int64>(FMath::Clamp(Chunk, -9.2e18, 9.2e18));
	}

	// The cell along one axis inside a chunk, kept 0 to 15 where rounding lands on an edge.
	int32 CellAlong(double Location, int64 Chunk, double ChunkSize)
	{
		const double Offset = Location - static_cast<double>(Chunk) * ChunkSize;
		return static_cast<int32>(FMath::Clamp(FMath::FloorToDouble(Offset / (ChunkSize / CellsPerAxis)), 0.0, CellsPerAxis - 1.0));
	}

	// A grid cell shows an edit when the edit sits inside the grid with a type that fits a byte.
	bool ShownByGrid(const FCrowdyVoxel& Edit)
	{
		return GridIndex(Edit.Position) != INDEX_NONE && Edit.Type >= 0 && Edit.Type <= MAX_uint8;
	}
}

FVector UCrowdyVoxelLibrary::GetChunkOrigin(const UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ)
{
	const double ChunkSize = ChunkSizeFor(WorldContextObject);
	return FVector(static_cast<double>(ChunkX) * ChunkSize, static_cast<double>(ChunkY) * ChunkSize, static_cast<double>(ChunkZ) * ChunkSize);
}

double UCrowdyVoxelLibrary::GetVoxelSize(const UObject* WorldContextObject)
{
	return ChunkSizeFor(WorldContextObject) / CellsPerAxis;
}

void UCrowdyVoxelLibrary::WorldToVoxel(const UObject* WorldContextObject, FVector Location, int64& ChunkX, int64& ChunkY, int64& ChunkZ, FIntVector& Position)
{
	const double ChunkSize = ChunkSizeFor(WorldContextObject);
	ChunkX = ChunkAlong(Location.X, ChunkSize);
	ChunkY = ChunkAlong(Location.Y, ChunkSize);
	ChunkZ = ChunkAlong(Location.Z, ChunkSize);
	Position = FIntVector(CellAlong(Location.X, ChunkX, ChunkSize), CellAlong(Location.Y, ChunkY, ChunkSize), CellAlong(Location.Z, ChunkZ, ChunkSize));
}

FVector UCrowdyVoxelLibrary::VoxelToWorld(const UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ, FIntVector Position)
{
	const double Size = GetVoxelSize(WorldContextObject);
	return GetChunkOrigin(WorldContextObject, ChunkX, ChunkY, ChunkZ) + (FVector(Position) + FVector(0.5)) * Size;
}

FCrowdyVoxel UCrowdyVoxelLibrary::GetVoxel(const FCrowdyVoxelChunk& Chunk, FIntVector Position)
{
	FCrowdyVoxel Voxel;
	Voxel.Position = Position;
	// Positions are 16-bit, and the search key keeps only 16 bits of each axis.
	const bool bFits = FMath::Max3(Position.X, Position.Y, Position.Z) <= MAX_int16 && FMath::Min3(Position.X, Position.Y, Position.Z) >= MIN_int16;
	if (!bFits)
	{
		return Voxel;
	}
	const int32 Found = Algo::BinarySearchBy(Chunk.Edits, PositionKey(Position), [](const FCrowdyVoxel& Edit) { return PositionKey(Edit.Position); });
	if (Found != INDEX_NONE)
	{
		return Chunk.Edits[Found];
	}
	const int32 Index = GridIndex(Position);
	if (Index != INDEX_NONE && Index < Chunk.Grid.Num())
	{
		Voxel.Type = Chunk.Grid[Index];
	}
	return Voxel;
}

TArray<FCrowdyVoxel> UCrowdyVoxelLibrary::GetFilledVoxels(const FCrowdyVoxelChunk& Chunk)
{
	const int32 Count = FMath::Min(Chunk.Grid.Num(), VoxelCount);
	const uint8* Types = Chunk.Grid.GetData();
	int32 Filled = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Filled += Types[Index] != 0;
	}
	const int32 FromGrid = Filled;
	for (const FCrowdyVoxel& Edit : Chunk.Edits)
	{
		Filled += !ShownByGrid(Edit) && Edit.Type != 0;
	}

	TArray<FCrowdyVoxel> Voxels;
	Voxels.Reserve(Filled);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (Types[Index] == 0)
		{
			continue;
		}
		FCrowdyVoxel& Voxel = Voxels.AddDefaulted_GetRef();
		Voxel.Position = FIntVector(Index & 15, (Index >> 4) & 15, Index >> 8);
		Voxel.Type = Types[Index];
	}
	// The grid's cells are in index order, so an edit's state finds its cell by search.
	for (const FCrowdyVoxel& Edit : Chunk.Edits)
	{
		if (Edit.Type == 0 || Edit.State.IsEmpty() || !ShownByGrid(Edit))
		{
			continue;
		}
		const int32 Cell = Algo::BinarySearchBy(TArrayView<FCrowdyVoxel>(Voxels.GetData(), FromGrid), GridIndex(Edit.Position),
			[](const FCrowdyVoxel& Voxel) { return GridIndex(Voxel.Position); });
		if (Cell != INDEX_NONE)
		{
			Voxels[Cell].State = Edit.State;
		}
	}
	for (const FCrowdyVoxel& Edit : Chunk.Edits)
	{
		if (Edit.Type != 0 && !ShownByGrid(Edit))
		{
			Voxels.Add(Edit);
		}
	}
	return Voxels;
}
