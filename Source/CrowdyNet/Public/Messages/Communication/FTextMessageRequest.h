#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyFrame.h"

/** Proximity text: the payload is the message as UTF-8 and nothing else, the form CrowdyJS and CrowdyCPP send. */
struct FTextMessageRequest : ICrowdyMessage
{
	// Not sent. The sender is the envelope's actor id; these remain so older callers still compile.
	int64 UserID = 0;
	FString Username;

	FString Message;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_TEXT_PACKET;
	}

	virtual FName GetTypeName() const override
	{
		return "Text Message Request";
	}

	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata();
		const FTCHARToUTF8 Converted(*Message);
		Data.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
		return Data;
	}

	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		return false;
	}

};
