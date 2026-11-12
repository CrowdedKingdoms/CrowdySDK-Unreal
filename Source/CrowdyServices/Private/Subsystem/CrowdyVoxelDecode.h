#pragma once

#include "CoreMinimal.h"
#include "Algo/Sort.h"

struct FCrowdyVoxelChunk;

namespace CrowdyVoxelDecode
{
	constexpr int32 CellsPerAxis = 16;
	constexpr int32 VoxelCount = CellsPerAxis * CellsPerAxis * CellsPerAxis;
	constexpr int32 MaxStateBytes = 1024;
	constexpr int32 MaxEditsPerChunk = 65536;

	/** The most chunks one box read may span, which is the platform's own bound (a cube of range 8). */
	constexpr int32 MaxBoxChunks = 4913;

	/**
	 * Appends the chunks of one getChunksByDistance `data` object, as MessagePack, to Out, dropping any whose
	 * coordinates are already in Seen. Returns how many chunks the page held, or INDEX_NONE when it is malformed or
	 * holds more than MaxChunks. OutLimit is the page size the platform answered with, at most MaxChunks.
	 */
	int32 DecodePage(TConstArrayView<uint8> Msgpack, int32 MaxChunks, TArray<FCrowdyVoxelChunk>& Out, TSet<FInt64Vector>& Seen, int32& OutLimit);

	/**
	 * Appends the chunk groups of one listVoxelUpdatesByDistance `data` object as unstored chunks: an empty grid with
	 * the group's edits applied oldest first. A group whose coordinates are in Seen, or that holds no edit, is skipped;
	 * a chunk keeps its newest 65,536 edits and is marked truncated past that. Returns and bounds as DecodePage.
	 */
	int32 DecodeEditsPage(TConstArrayView<uint8> Msgpack, int32 MaxChunks, TArray<FCrowdyVoxelChunk>& Out, TSet<FInt64Vector>& Seen, int32& OutLimit);

	/** Reads one getChunk `data` object: 1 with the chunk in Out, 0 when the platform holds none, INDEX_NONE when malformed. */
	int32 DecodeOne(TConstArrayView<uint8> Msgpack, FCrowdyVoxelChunk& Out);

	/** Orders positions z, then y, then x, which is the grid's index order for the cells inside it. */
	inline uint64 PositionKey(const FIntVector& Position)
	{
		const auto Lane = [](int32 Axis) { return static_cast<uint64>(static_cast<uint16>(Axis ^ 0x8000)); };
		return (Lane(Position.Z) << 32) | (Lane(Position.Y) << 16) | Lane(Position.X);
	}

	/** The grid index of a position inside 0 to 15 on every axis, else INDEX_NONE. */
	inline int32 GridIndex(const FIntVector& Position)
	{
		const bool bInside = !((Position.X | Position.Y | Position.Z) & ~(CellsPerAxis - 1));
		return bInside ? Position.X + CellsPerAxis * Position.Y + CellsPerAxis * CellsPerAxis * Position.Z : INDEX_NONE;
	}

	/** Chunks along each axis of the box Low to High (Low no greater on any axis), or zero on an axis longer than MaxBoxChunks. */
	inline FIntVector BoxSize(const FInt64Vector& Low, const FInt64Vector& High)
	{
		const auto Axis = [](int64 From, int64 To)
		{
			const uint64 Span = static_cast<uint64>(To) - static_cast<uint64>(From);
			return From <= To && Span < MaxBoxChunks ? static_cast<int32>(Span) + 1 : 0;
		};
		return FIntVector(Axis(Low.X, High.X), Axis(Low.Y, High.Y), Axis(Low.Z, High.Z));
	}

	/** How many chunks a box of that size holds, or 0 when it is empty or holds more than MaxBoxChunks. */
	inline int32 BoxCount(const FIntVector& Size)
	{
		const int64 Count = static_cast<int64>(Size.X) * Size.Y * Size.Z;
		return Count <= MaxBoxChunks ? static_cast<int32>(Count) : 0;
	}

	/** The Index-th chunk of the box, x fastest, then y, then z. */
	inline FInt64Vector BoxCell(const FInt64Vector& Low, const FIntVector& Size, int32 Index)
	{
		return Low + FInt64Vector(Index % Size.X, (Index / Size.X) % Size.Y, Index / (Size.X * Size.Y));
	}

	inline bool InBox(const FInt64Vector& Coord, const FInt64Vector& Low, const FInt64Vector& High)
	{
		return Coord.X >= Low.X && Coord.X <= High.X && Coord.Y >= Low.Y && Coord.Y <= High.Y && Coord.Z >= Low.Z && Coord.Z <= High.Z;
	}

	inline bool AllInSet(const TSet<FInt64Vector>& Cells, const FInt64Vector& Low, const FInt64Vector& High)
	{
		const FIntVector Size = BoxSize(Low, High);
		const int32 Count = BoxCount(Size);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (!Cells.Contains(BoxCell(Low, Size, Index)))
			{
				return false;
			}
		}
		return Count > 0;
	}

	/**
	 * Empties Cells into boxes of at most MaxChunks chunks that hold nothing outside the set, lowest first. Each grows along
	 * x to half the cap, so a 17-wide band packs two rows a box, then along y, then z.
	 */
	inline void SplitIntoBoxes(TSet<FInt64Vector>& Cells, int32 MaxChunks, TFunctionRef<void(const FInt64Vector& Low, const FInt64Vector& High)> Emit)
	{
		TArray<FInt64Vector> Order = Cells.Array();
		Algo::Sort(Order, [](const FInt64Vector& A, const FInt64Vector& B)
		{
			return A.Z != B.Z ? A.Z < B.Z : A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
		});
		const int64 Cap = FMath::Max(MaxChunks, 1);
		const int64 MaxWidth = FMath::Max<int64>(Cap / 2, 1);
		for (const FInt64Vector& Low : Order)
		{
			if (!Cells.Contains(Low))
			{
				continue;
			}
			FInt64Vector High = Low;
			while (High.X - Low.X + 1 < MaxWidth && High.X < MAX_int64 && Cells.Contains(FInt64Vector(High.X + 1, Low.Y, Low.Z)))
			{
				++High.X;
			}
			const int64 Width = High.X - Low.X + 1;
			while ((High.Y - Low.Y + 2) * Width <= Cap && High.Y < MAX_int64
				&& AllInSet(Cells, FInt64Vector(Low.X, High.Y + 1, Low.Z), FInt64Vector(High.X, High.Y + 1, Low.Z)))
			{
				++High.Y;
			}
			const int64 Face = Width * (High.Y - Low.Y + 1);
			while ((High.Z - Low.Z + 2) * Face <= Cap && High.Z < MAX_int64
				&& AllInSet(Cells, FInt64Vector(Low.X, Low.Y, High.Z + 1), FInt64Vector(High.X, High.Y, High.Z + 1)))
			{
				++High.Z;
			}
			const FIntVector Size = BoxSize(Low, High);
			const int32 Count = BoxCount(Size);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Cells.Remove(BoxCell(Low, Size, Index));
			}
			Emit(Low, High);
		}
	}
}
