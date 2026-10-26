#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyFrame.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * A client event whose state follows the event type directly: the two-octet type, then the state to the end
 * of the message, with no length and no routing block. This is the layout the Game Kit's events use in every
 * client library, so a request of this type is read by CrowdyCPP and CrowdyJS as written. Send-only.
 */
struct FKitEventRequest : ICrowdyMessage
{
	uint16 EventType = 0;

	TArray<uint8> State;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Kit Event Request";
	}

	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata(static_cast<int32>(sizeof(EventType)) + State.Num());
		USerializationFunctionLibrary::AppendValue(Data, EventType);
		Data.Append(State);
		return Data;
	}

	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		return false;
	}
};
