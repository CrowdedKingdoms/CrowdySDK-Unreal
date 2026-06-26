#pragma once
#include "Core/GraphQL/Interfaces/ICrowdyQueryResponse.h"
#include "Core/GraphQL/Enums/EQueryResponseType.h"
#include "Misc/Base64.h"

/**
 * Parsed response from getVoxelList when issued by UCrowdyPersistenceSubsystem.
 *
 * A persistence chunk holds one voxel per struct instance, keyed by Vx
 * (CRC32 of the instance key, mapped to 1..32767; 0 = singleton).
 * VoxelStateBytes maps Vx → raw serialized struct bytes.
 */
struct FPersistencePullResponse : ICrowdyQueryResponse
{
	/** Chunk coordinates echoed from the response — used for routing. */
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;

	/** Vx → raw serialized struct bytes (Base64-decoded from 'state' field). */
	TMap<int16, TArray<uint8>> VoxelStateBytes;

	/** Vx → VoxelType (= struct TypeID stored during push). */
	TMap<int16, int16> VoxelTypes;

	virtual EQueryResponseType GetResponseType() const override
	{
		return EQueryResponseType::PersistencePull;
	}

	virtual FName GetOperationName() const override
	{
		return TEXT("Persistence Pull Response");
	}

	virtual void ParseResponse(const TSharedPtr<FJsonObject>& JsonObject) override
	{
		bIsValid = false;

		if (!JsonObject.IsValid())
		{
			ErrorMessage = TEXT("Invalid JSON object.");
			return;
		}

		const TSharedPtr<FJsonObject>* DataObj;
		if (!JsonObject->TryGetObjectField(TEXT("data"), DataObj) || !DataObj->IsValid())
		{
			ErrorMessage = TEXT("Missing 'data' field.");
			return;
		}

		const TSharedPtr<FJsonObject>* VoxelListObj;
		if (!(*DataObj)->TryGetObjectField(TEXT("getVoxelList"), VoxelListObj) || !VoxelListObj->IsValid())
		{
			ErrorMessage = TEXT("Missing 'getVoxelList' field.");
			return;
		}

		// ── Chunk coordinates ─────────────────────────────────────────────────
		const TSharedPtr<FJsonObject>* CoordsObj;
		if ((*VoxelListObj)->TryGetObjectField(TEXT("coordinates"), CoordsObj))
		{
			FString Sx, Sy, Sz;
			(*CoordsObj)->TryGetStringField(TEXT("x"), Sx);
			(*CoordsObj)->TryGetStringField(TEXT("y"), Sy);
			(*CoordsObj)->TryGetStringField(TEXT("z"), Sz);
			ChunkX = FCString::Atoi64(*Sx);
			ChunkY = FCString::Atoi64(*Sy);
			ChunkZ = FCString::Atoi64(*Sz);
		}

		// ── Voxels ────────────────────────────────────────────────────────────
		const TArray<TSharedPtr<FJsonValue>>* VoxelArray;
		if (!(*VoxelListObj)->TryGetArrayField(TEXT("voxels"), VoxelArray) || VoxelArray->IsEmpty())
		{
			// Empty chunk — valid, just no data pushed yet.
			bIsValid = true;
			return;
		}

		for (const TSharedPtr<FJsonValue>& VoxelVal : *VoxelArray)
		{
			const TSharedPtr<FJsonObject>* VoxelObj;
			if (!VoxelVal->TryGetObject(VoxelObj) || !VoxelObj->IsValid())
				continue;

			// location.x = Vx (instance slot)
			const TSharedPtr<FJsonObject>* LocObj;
			if (!(*VoxelObj)->TryGetObjectField(TEXT("location"), LocObj))
				continue;

			FString Sx;
			(*LocObj)->TryGetStringField(TEXT("x"), Sx);
			const int16 Vx = static_cast<int16>(FCString::Atoi(*Sx));

			// VoxelType = struct TypeID stored during push
			FString VoxelTypeStr;
			if ((*VoxelObj)->TryGetStringField(TEXT("voxelType"), VoxelTypeStr))
				VoxelTypes.Add(Vx, static_cast<int16>(FCString::Atoi(*VoxelTypeStr)));

			// State = Base64-encoded serialized struct bytes
			FString StateB64;
			if (!(*VoxelObj)->TryGetStringField(TEXT("state"), StateB64))
				continue;

			TArray<uint8> Decoded;
			if (FBase64::Decode(StateB64, Decoded) && !Decoded.IsEmpty())
				VoxelStateBytes.Add(Vx, MoveTemp(Decoded));
		}

		bIsValid = true;
	}
};
