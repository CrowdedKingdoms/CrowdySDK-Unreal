#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"


struct FClientAudioNotificationFrame
{
	int32 FrameSize = 0;
	TArray<uint8> AudioData;
};

struct FClientAudioNotification : ICrowdyMessage
{
	int32 SampleRate;
	int32 NumChannels;
	
	TArray<FClientAudioNotificationFrame> Frames;
	
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION;
	}
	
	virtual FName GetTypeName() const override
	{
		return "Client Audio Notification";
	}
	
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}
	
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		int32 Offset = 0;

		if (!DeserializeMetadata(Data, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FClientAudioNotification::Deserialize - Metadata deserialization failed"));
			return false;
		}

		// SampleRate
		if (!USerializationFunctionLibrary::DeserializeValue(Data, SampleRate, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FClientAudioNotification::Deserialize - SampleRate deserialization failed"));
			return false;
		}
		Offset += sizeof(int32);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, NumChannels, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FClientAudioNotification::Deserialize - NumChannels deserialization failed"));
			return false;
		}
		Offset += sizeof(int32);
		
		int32 FrameCount = 0;
		if (!USerializationFunctionLibrary::DeserializeValue(Data, FrameCount, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FClientAudioNotification::Deserialize - FrameCount deserialization failed"));
			return false;
		}
		Offset += sizeof(int32);
		

		if (FrameCount <= 0 || FrameCount > 100)
		{
			Frames.Empty();
			return false;
		}

		Frames.Empty(FrameCount);

		for (int32 i = 0; i < FrameCount; ++i)
		{
			if (Offset + sizeof(int32) > Data.Num())
			{
				break;
			}

			FClientAudioNotificationFrame Frame;
			FMemory::Memcpy(&Frame.FrameSize, Data.GetData() + Offset, sizeof(int32));
			Offset += sizeof(int32);

			if (Frame.FrameSize <= 0 || Offset + Frame.FrameSize > Data.Num())
			{
				return false;
			}

			Frame.AudioData.SetNumUninitialized(Frame.FrameSize);
			FMemory::Memcpy(Frame.AudioData.GetData(), Data.GetData() + Offset, Frame.FrameSize);
			Offset += Frame.FrameSize;

			Frames.Add(MoveTemp(Frame));
		}
		return true;
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
};