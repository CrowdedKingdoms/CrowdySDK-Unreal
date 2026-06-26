#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Utils/SerializationFunctionLibrary.h"

struct FClientAudioFrame
{
	TArray<uint8> AudioData;
};

struct FClientAudioPacketMessageRequest : ICrowdyMessage
{
	
	int32 SampleRate;
	int32 NumChannels;
	TArray<FClientAudioFrame> Frames;
	
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_AUDIO_PACKET;
	}
	
	virtual FName GetTypeName() const override
	{
		return "Client Audio Packet Message Request";
	}
	
	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata();
		
		Data.Append(reinterpret_cast<const uint8*>(&SampleRate), sizeof(int32));
		Data.Append(reinterpret_cast<const uint8*>(&NumChannels), sizeof(int32));
		
		const int32 FrameCount = Frames.Num();
		Data.Append(reinterpret_cast<const uint8*>(&FrameCount), sizeof(int32));
		
		for (const FClientAudioFrame& Frame : Frames)
		{
			const int32 Lcl_FrameSize = Frame.AudioData.Num();
			Data.Append(reinterpret_cast<const uint8*>(&Lcl_FrameSize), sizeof(int32));
			Data.Append(Frame.AudioData);
		}
		
		return Data;
	}
	
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		return false;
	}
	
	virtual uint32 GetMessageSize() const override
	{
		uint32 Size = 0;

		Size += sizeof(int64);      // MapID
		Size += sizeof(int64) * 3;  // ChunkCoords
		Size += 32;                 // UUID
		Size += sizeof(int32);      // SampleRate
		Size += sizeof(int32);      // NumChannels
		Size += sizeof(int32);      // FrameCount

		return Size;
	}
	
	int32 GetAccumulatedMessageSize()
	{
		int32 Size = 0;

		Size += sizeof(int64);           // MapID
		Size += sizeof(int64) * 3;       // ChunkCoords
		Size += 32;                      // UUID
		Size += sizeof(int32);           // SampleRate
		Size += sizeof(int32);           // NumChannels
		Size += sizeof(int32);           // FrameCount
		for (const FClientAudioFrame& Frame : Frames)
		{
			Size += sizeof(int32);        // FrameSize field
			Size += Frame.AudioData.Num();// Frame bytes
		}
		return Size;
	}
};