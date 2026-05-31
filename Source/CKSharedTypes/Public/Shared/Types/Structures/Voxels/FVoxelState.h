#pragma once

#include "CoreMinimal.h"
#include "Runtime/Core/Public/UObject/NameTypes.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "FVoxelState.generated.h"


USTRUCT(BlueprintType, Blueprintable)
struct FPlacedObjectState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Voxel State | Game Objects")
	FString ObjectID;

	UPROPERTY(BlueprintReadWrite, Category = "Voxel State | Game Objects")
	FVector Location;

	UPROPERTY(BlueprintReadWrite, Category = "Voxel State | Game Objects")
	FRotator Rotation;

	UPROPERTY(BlueprintReadWrite, Category = "Voxel State | Game Objects")
	FVector Scale;

	FString ToString() const
	{
		return FString::Printf(
			TEXT("ObjectID=%s, Loc=(%s), Rot=(%s), Scale=(%s)"),
			*ObjectID,
			*Location.ToString(),
			*Rotation.ToString(),
			*Scale.ToString()
		);
	}

	// --- Helper for safe serialization ---
	template <typename T>
	static void WriteValue(FArchive& Ar, const T& Value)
	{
		T Temp = Value; // make a mutable copy
		Ar << Temp;
	}

	template <typename T>
	static void ReadValue(FArchive& Ar, T& Value)
	{
		Ar << Value;
	}

	// --- Serialization helpers ---
	TArray<uint8> Serialize() const
	{
		TArray<uint8> Data;
		FMemoryWriter Writer(Data, true);

		WriteValue(Writer, ObjectID);
		WriteValue(Writer, Location);
		WriteValue(Writer, Rotation);
		WriteValue(Writer, Scale);

		return Data;
	}

	void Deserialize(const TArray<uint8>& InData, int32& Offset)
	{
		if (Offset < 0 || Offset > InData.Num())
		{
			return;
		}
		FMemoryReader Reader(InData, true);
		Reader.Seek(Offset);

		ReadValue(Reader, ObjectID);
		ReadValue(Reader, Location);
		ReadValue(Reader, Rotation);
		ReadValue(Reader, Scale);

		Offset = Reader.Tell();
	}

	bool operator==(const FPlacedObjectState& Other) const
	{
		return ObjectID == Other.ObjectID
			&& Location == Other.Location
			&& Rotation == Other.Rotation
			&& Scale == Other.Scale;
	}
};

// --- Hash support (optional but useful) ---
FORCEINLINE uint32 GetTypeHash(const FPlacedObjectState& State)
{
	uint32 Hash = GetTypeHash(State.ObjectID);
	Hash = HashCombine(Hash, GetTypeHash(State.Location));
	Hash = HashCombine(Hash, GetTypeHash(State.Scale));

	Hash = HashCombine(Hash, GetTypeHash(State.Rotation.Pitch));
	Hash = HashCombine(Hash, GetTypeHash(State.Rotation.Yaw));
	Hash = HashCombine(Hash, GetTypeHash(State.Rotation.Roll));

	return Hash;
}

USTRUCT(BlueprintType, Blueprintable)
struct FVoxelState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category="Voxel State")
	uint8 Version;

	UPROPERTY(BlueprintReadWrite, Category="Voxel State");
	uint8 FaceOneDirection;

	UPROPERTY(BlueprintReadWrite, Category="Voxel State")
	uint8 Rotation;

	UPROPERTY(BlueprintReadWrite, Category="Voxel State")
	int64 AtlasOverride;

	UPROPERTY(BlueprintReadWrite, Category="Voxel State")
	bool bIsVLO;

	UPROPERTY(BlueprintReadWrite, Category="Voxel State | Game Objects")
	TArray<FPlacedObjectState> GameObjects;
	
	UPROPERTY(BlueprintReadWrite, Category="Voxel State | Voxel Edit Type")
	uint8 VoxelEditShape;
	
	UPROPERTY(BlueprintReadWrite, Category="Voxel State | Voxel Edit Type")
	FVector3f VoxelEditShapeSize;

	FVoxelState() :
		Version(5),
		FaceOneDirection(0),
		Rotation(0),
		AtlasOverride(0),
		bIsVLO(false),
		VoxelEditShape(0),
		VoxelEditShapeSize(FVector3f::Zero())
	{
	}

	bool IsVLO() const { return bIsVLO; }

	uint8 GetVersion() const { return Version; }

	FString ToString() const
	{
		FString GameObjectsInfo;

		GameObjectsInfo += FString::Printf(TEXT("GameObjects[%d]: "), GameObjects.Num());

		for (int32 i = 0; i < GameObjects.Num(); i++)
		{
			const FPlacedObjectState& Obj = GameObjects[i];
			GameObjectsInfo += FString::Printf(
				TEXT("{%d: %s} "),
				i,
				*Obj.ObjectID
			);
		}

		return FString::Printf(
			TEXT("FVoxelState(Version=%d, FaceOneDir=%d, Rot=%d, AtlasOverride=%lld, bIsVLO=%s, %s)"),
			Version,
			FaceOneDirection,
			Rotation,
			AtlasOverride,
			bIsVLO ? TEXT("true") : TEXT("false"),
			*GameObjectsInfo
		);
	}

	// Equality operator
	bool operator==(const FVoxelState& Other) const
	{
		return Version == Other.Version
			&& FaceOneDirection == Other.FaceOneDirection
			&& Rotation == Other.Rotation
			&& AtlasOverride == Other.AtlasOverride
			&& bIsVLO == Other.bIsVLO
			&& GameObjects == Other.GameObjects
			&& VoxelEditShape == Other.VoxelEditShape;
	}

	// Inequality operator (optional but useful)
	bool operator!=(const FVoxelState& Other) const
	{
		return !(*this == Other);
	}

	void ResetVoxelState()
	{
		Version = 5;
		FaceOneDirection = 0;
		Rotation = 0;
		AtlasOverride = 0;
		bIsVLO = false;
		VoxelEditShape = 0;
		VoxelEditShapeSize = FVector3f::Zero();
		//GameObjects.Empty();
	}

	// --- Tagged Chunk Serialization ---
	enum class EFieldID : uint32
	{
		Version = 1,
		FaceDirection = 2,
		Rotation = 3,
		AtlasOverride = 4,
		IsVLO = 5,
		GameObjects = 6,
		VoxelEditShape = 7,
		VoxelEditShapeSize = 8
	};

	// ... existing code ...
	TArray<uint8> SerializeToBytes() const
	{
		TArray<uint8> ByteArray;
		ByteArray.Reserve(1 + 8 * 5 + 64); // rough pre-alloc

		// New robust format begins with a single version byte.
		// This satisfies existing external checks that look at buffer[0].
		ByteArray.Add(Version);

		// Helper: write a tagged chunk header then payload bytes
		auto AppendChunkRaw = [&ByteArray](EFieldID FieldID, const void* Data, uint32 Size)
		{
			const uint32 ID = static_cast<uint32>(FieldID);
			ByteArray.Append(reinterpret_cast<const uint8*>(&ID), sizeof(uint32));
			ByteArray.Append(reinterpret_cast<const uint8*>(&Size), sizeof(uint32));
			if (Size > 0 && Data != nullptr)
			{
				ByteArray.Append(static_cast<const uint8*>(Data), Size);
			}
		};

		// Helper: write a trivially copyable value as a chunk
		auto AppendChunkValue = [&AppendChunkRaw]<typename TVal>(EFieldID FieldID, const TVal& V)
		{
			static_assert(std::is_trivially_copyable_v<TVal>, "AppendChunkValue requires trivially copyable type");
			AppendChunkRaw(FieldID, &V, sizeof(TVal));
		};

		// Serialize POD fields
		AppendChunkValue(EFieldID::Version, Version);
		AppendChunkValue(EFieldID::FaceDirection, FaceOneDirection);
		AppendChunkValue(EFieldID::Rotation, Rotation);
		AppendChunkValue(EFieldID::AtlasOverride, AtlasOverride);
		AppendChunkValue(EFieldID::IsVLO, bIsVLO);

		// Serialize GameObjects as: [Count:int32] then repeated [Size:int32][ObjBytes...]
		{
			TArray<uint8> ObjData;
			const int32 Count = GameObjects.Num();
			ObjData.Append(reinterpret_cast<const uint8*>(&Count), sizeof(int32));

			for (const auto& Obj : GameObjects)
			{
				const TArray<uint8> Single = Obj.Serialize();
				const int32 Size = Single.Num();
				ObjData.Append(reinterpret_cast<const uint8*>(&Size), sizeof(int32));
				if (Size > 0)
				{
					ObjData.Append(Single.GetData(), Size);
				}
				//UE_LOG(LogTemp, Verbose, TEXT("FVoxelState: Serialized GameObject: %s"), *Obj.ObjectID);
			}

			AppendChunkRaw(EFieldID::GameObjects, ObjData.GetData(), static_cast<uint32>(ObjData.Num()));
		}
		
		AppendChunkValue(EFieldID::VoxelEditShape, VoxelEditShape);
		AppendChunkValue(EFieldID::VoxelEditShapeSize, VoxelEditShapeSize);
		return ByteArray;
	}

	void DeserializeFromBytes(const TArray<uint8>& ByteArray, int32 Offset = 0)
	{
		const int32 TotalSize = ByteArray.Num();

		if (Offset < 0 || Offset > TotalSize)
		{
			return;
		}

		// Handle completely empty payload robustly
		if (TotalSize - Offset <= 0)
		{
			return;
		}

		const uint8* const Data = ByteArray.GetData();

		// Path A: New format: [Version:1][TaggedChunks...]
		// Many call sites check ByteArray[0] == ExpectedVersion.
		// We accept plausible version ranges [1..50] to be safe.
		{
			const uint8 Leading = Data[Offset];
			if (Leading >= 1 && Leading <= 50)
			{
				Version = Leading;
				Offset += 1;

				// Fall through to parse tagged chunks after the leading version byte
				// while keeping strong bounds checks.
				const int32 StartOfChunks = Offset;

				while (Offset + static_cast<int32>(sizeof(uint32) * 2) <= TotalSize)
				{
					uint32 FieldID = 0;
					uint32 Size = 0;

					FMemory::Memcpy(&FieldID, Data + Offset, sizeof(uint32));
					Offset += sizeof(uint32);
					FMemory::Memcpy(&Size, Data + Offset, sizeof(uint32));
					Offset += sizeof(uint32);

					const int64 Remaining = static_cast<int64>(TotalSize) - static_cast<int64>(Offset);
					if (Remaining < 0 || static_cast<int64>(Size) > Remaining)
					{
						break;
					}

					const uint8* ChunkPtr = Data + Offset;
					const uint32 ChunkSize = Size;

					Offset += static_cast<int32>(Size);

					switch (static_cast<EFieldID>(FieldID))
					{
					case EFieldID::Version:
						if (ChunkSize >= 1)
						{
							// If present, trust the inner version but keep the leading one if mismatch to avoid surprises
							const uint8 InnerVersion = ChunkPtr[0];
							if (InnerVersion >= 1 && InnerVersion <= 50)
							{
								Version = InnerVersion;
							}
						}
						break;

					case EFieldID::FaceDirection:
						if (ChunkSize >= 1)
						{
							FaceOneDirection = ChunkPtr[0];
						}
						break;

					case EFieldID::Rotation:
						if (ChunkSize >= 1)
						{
							Rotation = ChunkPtr[0];
						}
						break;

					case EFieldID::AtlasOverride:
						if (ChunkSize >= sizeof(int64))
						{
							FMemory::Memcpy(&AtlasOverride, ChunkPtr, sizeof(int64));
						}
						break;

					case EFieldID::IsVLO:
						if (ChunkSize >= sizeof(bool))
						{
							FMemory::Memcpy(&bIsVLO, ChunkPtr, sizeof(bool));
						}
						break;

					case EFieldID::GameObjects:
						{
							
							uint32 InnerOffset = 0;

							if (ChunkSize < sizeof(int32))
							{
								break;
							}

							int32 Count = 0;
							FMemory::Memcpy(&Count, ChunkPtr + InnerOffset, sizeof(int32));
							InnerOffset += sizeof(int32);

							if (Count < 0)
							{
								break;
							}

							constexpr int32 MaxReasonableObjects = 10000;
							if (Count > MaxReasonableObjects)
							{
								Count = MaxReasonableObjects;
							}

							GameObjects.Empty(Count);

							for (int32 i = 0; i < Count; i++)
							{
								if (InnerOffset + sizeof(int32) > ChunkSize)
								{
									break;
								}

								int32 ObjSize = 0;
								FMemory::Memcpy(&ObjSize, ChunkPtr + InnerOffset, sizeof(int32));
								InnerOffset += sizeof(int32);

								if (ObjSize < 0 || static_cast<uint64>(InnerOffset) + static_cast<uint64>(ObjSize) >
									static_cast<uint64>(ChunkSize))
								{
									break;
								}

								TArray<uint8> ObjData;
								if (ObjSize > 0)
								{
									ObjData.Append(ChunkPtr + InnerOffset, ObjSize);
								}
								InnerOffset += static_cast<uint32>(ObjSize);

								int32 ObjOffset = 0;
								FPlacedObjectState Obj;
								Obj.Deserialize(ObjData, ObjOffset);
								GameObjects.Add(MoveTemp(Obj));
							}
						}
						break;
						
					case EFieldID::VoxelEditShape:
						if (ChunkSize >= 1)
						{
							VoxelEditShape = ChunkPtr[0];
						}
						break;
					
					case EFieldID::VoxelEditShapeSize:
						if (ChunkSize >= sizeof(FVector3f))
						{
							FMemory::Memcpy(&VoxelEditShapeSize, ChunkPtr, sizeof(FVector3f));
						}
					default:
						
						break;
					}
				}

				// Done with Path A
				return;
			}
		}

		// Path B: Tagged-only legacy format (no leading version byte).
		// If at least one [ID|Size] header fits, try to parse it.
		if (Offset + static_cast<int32>(sizeof(uint32) * 2) <= TotalSize)
		{
			while (Offset + static_cast<int32>(sizeof(uint32) * 2) <= TotalSize)
			{
				uint32 FieldID = 0;
				uint32 Size = 0;

				FMemory::Memcpy(&FieldID, Data + Offset, sizeof(uint32));
				Offset += sizeof(uint32);
				FMemory::Memcpy(&Size, Data + Offset, sizeof(uint32));
				Offset += sizeof(uint32);

				const int64 Remaining = static_cast<int64>(TotalSize) - static_cast<int64>(Offset);
				if (Remaining < 0 || static_cast<int64>(Size) > Remaining)
				{
					
					break;
				}

				const uint8* ChunkPtr = Data + Offset;
				const uint32 ChunkSize = Size;

				Offset += static_cast<int32>(Size);

				switch (static_cast<EFieldID>(FieldID))
				{
				case EFieldID::Version:
					if (ChunkSize >= 1)
					{
						Version = ChunkPtr[0];
					}
					break;
				case EFieldID::FaceDirection:
					if (ChunkSize >= 1)
					{
						FaceOneDirection = ChunkPtr[0];
					}
					break;
				case EFieldID::Rotation:
					if (ChunkSize >= 1)
					{
						Rotation = ChunkPtr[0];
					}
					break;
				case EFieldID::AtlasOverride:
					if (ChunkSize >= sizeof(int64))
					{
						FMemory::Memcpy(&AtlasOverride, ChunkPtr, sizeof(int64));
					}
					break;
				case EFieldID::IsVLO:
					if (ChunkSize >= sizeof(bool))
					{
						FMemory::Memcpy(&bIsVLO, ChunkPtr, sizeof(bool));
					}
					break;
				case EFieldID::GameObjects:
					{
						uint32 InnerOffset = 0;
						if (ChunkSize < sizeof(int32))
						{
							
							break;
						}

						int32 Count = 0;
						FMemory::Memcpy(&Count, ChunkPtr + InnerOffset, sizeof(int32));
						InnerOffset += sizeof(int32);

						if (Count < 0)
						{
							
							break;
						}

						constexpr int32 MaxReasonableObjects = 10000;
						if (Count > MaxReasonableObjects)
						{
							
							Count = MaxReasonableObjects;
						}

						GameObjects.Empty(Count);

						for (int32 i = 0; i < Count; i++)
						{
							if (InnerOffset + sizeof(int32) > ChunkSize)
							{
								
								break;
							}

							int32 ObjSize = 0;
							FMemory::Memcpy(&ObjSize, ChunkPtr + InnerOffset, sizeof(int32));
							InnerOffset += sizeof(int32);

							if (ObjSize < 0 || static_cast<uint64>(InnerOffset) + static_cast<uint64>(ObjSize) >
								static_cast<uint64>(ChunkSize))
							{
								
								break;
							}

							TArray<uint8> ObjData;
							if (ObjSize > 0)
							{
								ObjData.Append(ChunkPtr + InnerOffset, ObjSize);
							}
							InnerOffset += static_cast<uint32>(ObjSize);

							int32 ObjOffset = 0;
							FPlacedObjectState Obj;
							Obj.Deserialize(ObjData, ObjOffset);
							GameObjects.Add(MoveTemp(Obj));
						}
					}
					break;

				default:
					// Unknown field - ignore for forwards compatibility
					break;
				}
			}
			return;
		}

		// Path C: Very old "flat" layout as a last resort:
		// [Version:1][Face:1][Rotation:1][Atlas:int64][IsVLO:1]
		{
			const int32 MinLegacy = 1 /*Version*/ + 1 /*Face*/ + 1 /*Rotation*/ + static_cast<int32>(sizeof(int64)) + 1
				/*IsVLO*/;
			if (TotalSize - Offset >= MinLegacy)
			{
				const uint8* Ptr = Data + Offset;
				const uint8 PossibleVersion = Ptr[0];
				if (PossibleVersion >= 1 && PossibleVersion <= 50)
				{
					Version = PossibleVersion;
					FaceOneDirection = Ptr[1];
					Rotation = Ptr[2];
					Ptr += 3;
					FMemory::Memcpy(&AtlasOverride, Ptr, sizeof(int64));
					Ptr += sizeof(int64);
					FMemory::Memcpy(&bIsVLO, Ptr, sizeof(bool));
					// GameObjects not present in this legacy format.
				}
				// else: not plausible, keep defaults
			}
		}
	}
};


