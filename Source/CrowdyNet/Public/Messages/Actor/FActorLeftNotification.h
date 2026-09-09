#pragma once

#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyFrame.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * The server has stopped considering an actor present.
 *
 * Sent once, over the actor's last chunk, and never for a move between chunks. It is the server's own
 * answer to "is this actor still there", as opposed to a client noticing it has heard nothing; a later
 * update for the same actor is a rejoin rather than an error.
 *
 * Everything the notification carries beyond the reason is the spatial envelope the base class already
 * holds: the actor id, the chunk it was last seen in, the server's timestamp and the sequence number.
 */
struct FActorLeftNotification : ICrowdyMessage
{
	/** The departing actor, as the 32 octets read as a key. */
	FGuid GUID;

	ECrowdyActorLeftReason Reason = ECrowdyActorLeftReason::Stale;

	/** The reason exactly as it arrived, so a value this build has no entry for is still readable. */
	uint8 RawReason = 0;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Actor Left Notification";
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
			UE_LOG(LogCrowdyNet, Warning, TEXT("FActorLeftNotification::DecodePayload - the frame carries no actor id"));
			return false;
		}

		GUID = USerializationFunctionLibrary::ToGuid(UUID);

		// A body-less notification is a valid one rather than a malformed frame: the reason is optional on
		// the wire and its absence means the plain stale drop.
		RawReason = Frame.Body.IsEmpty() ? 0 : Frame.Body[0];
		Reason = CrowdyActorLeftReasonFromPayload(Frame.Body);
		return true;
	}
};
