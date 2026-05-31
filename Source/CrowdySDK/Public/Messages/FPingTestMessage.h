#pragma once
#include "Core/UDP/Interfaces/ICrowdyMessage.h"

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
	
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		int32 Offset = 0;
		if (!DeserializeMetadata(Data, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FPingTestMessage::Deserialize - Metadata deserialization failed"));
			return false;
		}
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, SendTime, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FPingTestMessage::Deserialize - SendTime deserialization failed"));
			return false;
		}
		ReceiveTime = FDateTime::UtcNow().GetTicks()/10000;
		return true;
	}
	
	virtual uint32 GetMessageSize() const override
	{
		return 0;
	}
};
