#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Messages/Communication/FClientAudioBody.h"
#include "Serialization/CrowdyFrame.h"
#include "Utils/SerializationFunctionLibrary.h"

struct FClientAudioPacketMessageRequest : FClientAudioBody
{
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
		AppendBody(Data);
		return Data;
	}

	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		return false;
	}

	int32 GetAccumulatedMessageSize()
	{
		int32 Size = 0;

		Size += sizeof(int64);           // MapID
		Size += sizeof(int64) * 3;       // ChunkCoords
		Size += 32;                      // actor id
		Size += sizeof(int32);           // SampleRate
		Size += sizeof(int32);           // NumChannels
		Size += sizeof(int32);           // FrameCount
		for (const FCrowdyAudioFrame& Frame : Frames)
		{
			Size += sizeof(int32);        // FrameSize field
			Size += Frame.AudioData.Num();// Frame bytes
		}
		return Size;
	}
};
