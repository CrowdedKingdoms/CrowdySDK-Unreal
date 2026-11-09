#pragma once

#include "CoreMinimal.h"

struct FCrowdyVoxelChunk;

namespace CrowdyVoxelDecode
{
	constexpr int32 CellsPerAxis = 16;
	constexpr int32 VoxelCount = CellsPerAxis * CellsPerAxis * CellsPerAxis;

	/**
	 * Appends the chunks of one getChunksByDistance `data` object, as MessagePack, to Out, dropping any whose
	 * coordinates are already in Seen. Returns how many chunks the page held, or INDEX_NONE when it is malformed or
	 * holds more than MaxChunks. OutLimit is the page size the platform answered with, at most MaxChunks.
	 */
	int32 DecodePage(TConstArrayView<uint8> Msgpack, int32 MaxChunks, TArray<FCrowdyVoxelChunk>& Out, TSet<FInt64Vector>& Seen, int32& OutLimit);

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
}
