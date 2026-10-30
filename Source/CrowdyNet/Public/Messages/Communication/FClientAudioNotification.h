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
		Payload.Reset();

		ApplyEnvelope(Frame);
		if (!ApplyEnvelopeActorId(Frame))
		{
			UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log, TEXT("FClientAudioNotification::DecodePayload - the frame carries no actor id"));
			return false;
		}

		const TConstArrayView<uint8> Data = Frame.Body;
		if (Data.Num() < 1)
		{
			UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log, TEXT("FClientAudioNotification::DecodePayload - the audio payload is empty"));
			return false;
		}

		if (Data.Num() > CrowdyAudioPacket::MaxPayloadBytes)
		{
			UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log, TEXT("FClientAudioNotification::DecodePayload - the audio payload is %d octets, over the %d a datagram carries"),
				Data.Num(), CrowdyAudioPacket::MaxPayloadBytes);
			return false;
		}

		Payload.Append(Data.GetData(), Data.Num());
		return true;
	}
};