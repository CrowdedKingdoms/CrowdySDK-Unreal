#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Messages/GameObjects/FGameEventRequest.h"
#include "Messages/GameObjects/FGameEventNotification.h"

/**
 * Actor-to-actor variants of the game-event wire messages. They are byte-for-byte identical
 * to FGameEventRequest / FGameEventNotification — same metadata, payload, and tail — and only
 * change the opcode to SINGLE_ACTOR_MESSAGE. The server reads that opcode and delivers the
 * message to the single client that owns the destination actor (the metadata UUID) rather than
 * broadcasting it spatially. The sender sets UUID = the target actor and the chunk to the
 * target's chunk; there is no echo back to the sender.
 */
struct FSingleActorRequest : FGameEventRequest
{
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::SINGLE_ACTOR_MESSAGE;
	}

	virtual FName GetTypeName() const override
	{
		return "Single Actor Request";
	}
};

struct FSingleActorNotification : FGameEventNotification
{
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::SINGLE_ACTOR_MESSAGE;
	}

	virtual FName GetTypeName() const override
	{
		return "Single Actor Notification";
	}
};
