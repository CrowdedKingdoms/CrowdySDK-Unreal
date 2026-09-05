#pragma once
#include "CrowdyNetLog.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyFrame.h"

struct FPingTestMessage : ICrowdyMessage
{
	int64 SendTime = FDateTime::UtcNow().GetTicks()/10000;
	int64 ReceiveTime = 0;
	
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::GENERIC_SPATIAL_1;
	}
	
	virtual FName GetTypeName() const override
	{
		return "Ping Test Message";
	}
	
	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata();
		Data.Append(USerializationFunctionLibrary::SerializeValue(SendTime));
		return Data;
	}
	
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		const TConstArrayView<uint8> Data = Frame.Body;
		int32 Offset = 0;

		ApplyEnvelope(Frame);
		if (!ApplyEnvelopeActorId(Frame))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FPingTestMessage::DecodePayload - the frame carries no actor id"));
			return false;
		}


		if (!USerializationFunctionLibrary::DeserializeValue(Data, SendTime, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FPingTestMessage::DecodePayload - SendTime deserialization failed"));
			return false;
		}
		ReceiveTime = FDateTime::UtcNow().GetTicks()/10000;
		return true;
	}
	
};
