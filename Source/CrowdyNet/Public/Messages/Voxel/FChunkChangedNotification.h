#pragma once

#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyFrame.h"

/**
 * A hub or mod replaced a chunk as a whole, so a copy read before it is out of date; read the chunk again. The chunk is
 * the envelope's. Nothing else arrives for that change. Nothing in it is signed, so it is a hint to read, never data.
 */
struct FChunkChangedNotification : ICrowdyMessage
{
	/** The stored chunk's new update time in epoch milliseconds, or 0 when the payload is shorter than 8 octets. */
	int64 UpdatedAtMs = 0;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CHUNK_CHANGED_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Chunk Changed Notification";
	}

	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		ApplyEnvelope(Frame);
		if (!ApplyEnvelopeActorId(Frame))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FChunkChangedNotification::DecodePayload - the frame carries no envelope"));
			return false;
		}

		UpdatedAtMs = 0;
		if (Frame.Body.Num() < 8)
		{
			return true;
		}
		uint64 Value = 0;
		for (int32 Octet = 0; Octet < 8; ++Octet)
		{
			Value |= static_cast<uint64>(Frame.Body[Octet]) << (8 * Octet);
		}
		UpdatedAtMs = static_cast<int64>(Value);
		return true;
	}
};
