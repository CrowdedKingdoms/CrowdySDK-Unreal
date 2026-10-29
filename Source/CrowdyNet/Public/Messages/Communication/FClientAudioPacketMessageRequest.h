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
		TArray<uint8> Data = SerializeMetadata(Payload.Num());
		Data.Append(Payload);
		return Data;
	}

	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		return false;
	}
};
