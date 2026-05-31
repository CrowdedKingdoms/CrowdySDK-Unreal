#pragma once
#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Utils/SerializationFunctionLibrary.h"

constexpr  int32 MetadataSize =  sizeof(int64)    // MapID
		+ sizeof(int64)  // ChunkX
		+ sizeof(int64)  // ChunkY
		+ sizeof(int64)  // ChunkZ
		+ sizeof(uint8)  // ReplicationDistance
		+ sizeof(uint8)  // DecayRate
		+ sizeof(uint8)  // bContainsAuth
		+ 32;            // UUID string

constexpr int32 TailSize = sizeof(int64) + sizeof(uint8);
/**
 * Interface representing a generic message in the Crowdy system.
 */
class CROWDYSDK_API ICrowdyMessage
{
public:
	
	FString UUID;
	
	int64 AppID = 0;
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;
	int64 GameTokenID = 0;
	int64 Timestamp = 0;
	
	ECrowdyReplicationDistance ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks;
	ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay;
	uint8 SequenceNumber = 0;
	bool bContainsAuth = true;
	
public:
	
	virtual ~ICrowdyMessage() = default;
	
	virtual ECrowdyMessageType GetType() const = 0;
	
	virtual FName GetTypeName() const = 0;
	
	virtual TArray<uint8> Serialize() const = 0;
	
	virtual [[nodiscard]] bool Deserialize(const TArray<uint8>& Data) = 0;
	
	virtual uint32 GetMessageSize() const = 0;
	
protected:
	
	FORCEINLINE TArray<uint8> SerializeMetadata() const
	{
		TArray<uint8> Data;
		Data.Reserve(MetadataSize + 1); // +1 for type byte
		
		Data.Add(static_cast<uint8>(GetType()));
		Data.Append(USerializationFunctionLibrary::SerializeValue(AppID));
		Data.Append(USerializationFunctionLibrary::SerializeValue(ChunkX));
		Data.Append(USerializationFunctionLibrary::SerializeValue(ChunkY));
		Data.Append(USerializationFunctionLibrary::SerializeValue(ChunkZ));
		Data.Add(static_cast<uint8>(ReplicationDistance));
		Data.Add(static_cast<uint8>(DecayRate));
		Data.Add(bContainsAuth ? static_cast<uint8>(1) : static_cast<uint8>(0));

		const FTCHARToUTF8 ConvertedUUID(*UUID);
		Data.Append(reinterpret_cast<const uint8*>(ConvertedUUID.Get()), ConvertedUUID.Length());

		return Data;
	}
	
	FORCEINLINE [[nodiscard]] bool DeserializeMetadata(const TArray<uint8>& Data, int32& Offset)
	{
		if (!ensureMsgf(Data.Num() >= Offset + MetadataSize + TailSize, 
			TEXT("DeserializeMetadata: Buffer too small. Have %d bytes, need at least %d from offset %d"),
			Data.Num(), Offset + MetadataSize + TailSize, Offset))
		{
			return false;
		}
		
		int32 LocalOffset = Offset;
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, AppID, LocalOffset))
		{
			UE_LOG(LogTemp, Error, TEXT("DeserializeMetadata: Failed to deserialize MapID"));	
			return false;
		}
		
		LocalOffset += sizeof(int64);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, ChunkX, LocalOffset))
		{
			UE_LOG(LogTemp, Error, TEXT("DeserializeMetadata: Failed to deserialize ChunkX"));	
			return false;
		}
		
		LocalOffset+= sizeof(int64);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, ChunkY, LocalOffset))
		{
			UE_LOG(LogTemp, Error, TEXT("DeserializeMetadata: Failed to deserialize ChunkY"))
			return false;
		}
		
		LocalOffset += sizeof(int64);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, ChunkZ, LocalOffset))
		{
			UE_LOG(LogTemp, Error, TEXT("DeserializeMetadata: Failed to read ChunkZ at offset %d"), LocalOffset);
			return false;
		}
		LocalOffset += sizeof(int64);
		
		ReplicationDistance = static_cast<ECrowdyReplicationDistance>(Data[LocalOffset]);
		LocalOffset += sizeof(uint8);

		DecayRate = static_cast<ECrowdyDecayRate>(Data[LocalOffset]);
		LocalOffset += sizeof(uint8);

		bContainsAuth = Data[LocalOffset] != 0;
		LocalOffset += sizeof(uint8);
		
		FString DeserializedUUID = USerializationFunctionLibrary::DeserializeString(Data, LocalOffset, 32);
		if (!ensureMsgf(
			DeserializedUUID.Len() == 32,
			TEXT("DeserializeMetadata: UUID deserialization returned unexpected length %d (expected 32)"),
			DeserializedUUID.Len()))
		{
			return false;
		}
		
		UUID = MoveTemp(DeserializedUUID);
		LocalOffset += 32;
		
		Offset = LocalOffset;
		
		return ExtractTailData(Data);
	}
	
private:
	
	FORCEINLINE [[nodiscard]] bool ExtractTailData(const TArray<uint8>& Data)
	{
		if (!ensureMsgf(
			Data.Num() >= TailSize,
			TEXT("ExtractTailData: Buffer too small for tail. Have %d bytes, need %d."),
			Data.Num(), TailSize))
		{
			return false;
		}
		
		// Tail is always anchored to the end of the buffer regardless of payload size
		int32 TempOffset = Data.Num() - TailSize;
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, Timestamp, TempOffset))
		{
			UE_LOG(LogTemp, Error, TEXT("ExtractTailData: Failed to read Timestamp at offset %d"), TempOffset);
			return false;
		}
		TempOffset += sizeof(int64);
		
		// Final byte is within the validated tail window
		SequenceNumber = Data[TempOffset];

		return true;
		
	}
	
};
