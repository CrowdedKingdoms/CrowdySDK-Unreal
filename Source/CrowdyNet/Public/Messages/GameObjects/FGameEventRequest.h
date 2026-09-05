#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Messages/GameObjects/FGameEventBody.h"
#include "Serialization/CrowdyFrame.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * Wire message carrying a game event payload (an FInstancedStruct) from this
 * client to the relay server; send-only - deserialization is handled by
 * FGameEventNotification.
 *
 * Target/TargetID are appended after the payload so relays and old clients
 * that only understand the legacy layout pass them through / ignore them.
 */
struct FGameEventRequest : FGameEventBody
{
	/** The request owns the octets it sends, so the shared body's buffer is public on this side. */
	using FGameEventBody::StateBytes;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Game Event Request";
	}

	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata(BodySize());
		if (!AppendBody(Data))
		{
			// The send path refuses a message that serializes to nothing, which is how a payload that
			// could not be encoded is dropped rather than sent malformed.
			return TArray<uint8>();
		}

		return Data;
	}

	// Send-only message - nothing to decode.
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		return false;
	}
};
