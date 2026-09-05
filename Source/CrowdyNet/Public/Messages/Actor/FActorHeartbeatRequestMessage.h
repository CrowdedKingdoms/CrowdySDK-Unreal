#pragma once
#include "CrowdyNetLog.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Serialization/CrowdyFrame.h"

/**
 * @class FActorHeartbeatRequestMessage
 * @brief Says an actor is still present, without restating its state.
 *
 * An actor that has not changed still has to be heard from, or the server treats it as gone. Sending its whole
 * state again to say so costs the full payload every time; this message carries the spatial header and nothing
 * after it, so an idle actor stays present for a fraction of the traffic.
 *
 * Send-only. Nothing arrives under this opcode: presence reaches other clients as an ordinary actor update
 * notification.
 */
struct FActorHeartbeatRequestMessage : ICrowdyMessage
{
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_ACTOR_HEARTBEAT;
	}

	virtual FName GetTypeName() const override
	{
		return "Actor Heartbeat Request Message";
	}

	/** The spatial header alone. A heartbeat with a payload is refused by the transport rather than trimmed. */
	virtual TArray<uint8> Serialize() const override
	{
		return SerializeMetadata();
	}

	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		UE_LOG(LogCrowdyNet, Warning,
			TEXT("FActorHeartbeatRequestMessage::DecodePayload called, but nothing arrives under this opcode."));
		return false;
	}
};
