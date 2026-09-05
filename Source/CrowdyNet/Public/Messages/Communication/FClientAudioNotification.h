#pragma once
#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Messages/Communication/FClientAudioBody.h"
#include "Serialization/CrowdyFrame.h"
#include "Utils/SerializationFunctionLibrary.h"

struct FClientAudioNotification : FClientAudioBody
{
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
	
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		const TConstArrayView<uint8> Data = Frame.Body;
		int32 Offset = 0;

		ApplyEnvelope(Frame);
		if (!ApplyEnvelopeActorId(Frame))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FClientAudioNotification::DecodePayload - the frame carries no actor id"));
			return false;
		}

		// SampleRate
		if (!USerializationFunctionLibrary::DeserializeValue(Data, SampleRate, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FClientAudioNotification::DecodePayload - SampleRate deserialization failed"));
			return false;
		}
		Offset += sizeof(int32);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, NumChannels, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FClientAudioNotification::DecodePayload - NumChannels deserialization failed"));
			return false;
		}
		Offset += sizeof(int32);
		
		int32 FrameCount = 0;
		if (!USerializationFunctionLibrary::DeserializeValue(Data, FrameCount, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FClientAudioNotification::DecodePayload - FrameCount deserialization failed"));
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

			FCrowdyAudioFrame AudioFrame;
			FMemory::Memcpy(&AudioFrame.FrameSize, Data.GetData() + Offset, sizeof(int32));
			Offset += sizeof(int32);

			// Written as a comparison against the bytes remaining rather than as an addition, because a
			// large declared frame size would overflow the sum and produce a negative value that passes.
			if (AudioFrame.FrameSize <= 0 || AudioFrame.FrameSize > Data.Num() - Offset)
			{
				return false;
			}

			AudioFrame.AudioData.SetNumUninitialized(AudioFrame.FrameSize);
			FMemory::Memcpy(AudioFrame.AudioData.GetData(), Data.GetData() + Offset, AudioFrame.FrameSize);
			Offset += AudioFrame.FrameSize;

			Frames.Add(MoveTemp(AudioFrame));
		}
		return true;
	}
};