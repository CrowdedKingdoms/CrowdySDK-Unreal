#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Core/GraphQL/Enums/EGraphQLQuery.h"
#include "Misc/Base64.h"

/**
 * GraphQL mutation used internally by UCrowdyPersistenceSubsystem to write
 * a single struct instance's state to the server.
 *
 * Encodes the voxel slot + serialized struct bytes into a binary blob and
 * sends it via the UpdateChunk mutation. Uses an inline query body so no
 * data-asset entry is required.
 *
 * Blob layout (before Base64):
 *   [int16 Vx][int16 Vy][int16 Vz][int16 VoxelType][uint16 StateSize][StateBytes…]
 */
struct FPersistencePushRequest : ICrowdyQueryRequest
{
	int64  AppID     = -1;
	int64  ChunkX    = 0;   // struct TypeID
	int64  ChunkY    = 0;   // CRC32(InstanceKey) — 0 for singletons
	int64  ChunkZ    = 0;
	// Vx / Vy / Vz are always 0: each instance owns its own chunk,
	// so there is never more than one voxel to write per request.
	int16  VoxelType = 0;   // struct TypeID — stored for verification on pull
	TArray<uint8> StateBytes;

	virtual FName GetOperationName() const override
	{
		return TEXT("Persistence Push Request");
	}

	virtual EGraphQLQuery GetQueryType() const override
	{
		return EGraphQLQuery::PersistencePush;
	}

	virtual void PrepareQuery() override
	{
		// Build the voxel blob: header fields then raw state bytes.
		// Vx / Vy / Vz are always 0 — each instance has its own chunk.
		TArray<uint8> Blob;
		AppendInt16(Blob, 0);          // Vx
		AppendInt16(Blob, 0);          // Vy
		AppendInt16(Blob, 0);          // Vz
		AppendInt16(Blob, VoxelType);
		const uint16 StateSize = static_cast<uint16>(StateBytes.Num());
		Blob.Append(reinterpret_cast<const uint8*>(&StateSize), sizeof(uint16));
		Blob.Append(StateBytes);

		RuntimeVariables.Empty();
		RuntimeVariables.Add(TEXT("appId"),  FString::Printf(TEXT("%lld"), AppID));
		RuntimeVariables.Add(TEXT("x"),      FString::Printf(TEXT("%lld"), ChunkX));
		RuntimeVariables.Add(TEXT("y"),      FString::Printf(TEXT("%lld"), ChunkY));
		RuntimeVariables.Add(TEXT("z"),      FString::Printf(TEXT("%lld"), ChunkZ));
		RuntimeVariables.Add(TEXT("voxels"), FBase64::Encode(Blob));

		InlineQueryBody   = GetStaticQueryBody();
		bIncludeAuthToken = true;
		bUseNestedJson    = false;
		bIsValid          = true;
	}

	virtual bool IsValid() const override
	{
		return AppID > -1 && StateBytes.Num() > 0 && RuntimeVariables.Num() == 5;
	}

private:

	static void AppendInt16(TArray<uint8>& Buf, int16 V)
	{
		Buf.Append(reinterpret_cast<const uint8*>(&V), sizeof(int16));
	}

	static const FString& GetStaticQueryBody()
	{
		static const FString Body =
			TEXT("mutation UpdateChunk($appId: BigInt!, $x: BigInt!, $y: BigInt!, $z: BigInt!, $voxels: String!) {")
			TEXT("  updateChunk(input: {appId: $appId, coordinates: {x: $x, y: $y, z: $z}, voxels: $voxels}) {")
			TEXT("    chunkId")
			TEXT("    coordinates { x y z }")
			TEXT("  }")
			TEXT("}");
		return Body;
	}
};
