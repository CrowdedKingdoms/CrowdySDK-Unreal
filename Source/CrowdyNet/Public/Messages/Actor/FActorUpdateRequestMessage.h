#pragma once
#include "CrowdyNetLog.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Messages/Actor/FActorUpdateBody.h"
#include "Serialization/CrowdyFrame.h"
#include "Shared/Types/Structures/Actors/FActorState.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * @class FActorUpdateRequestMessage
 * @brief Represents a message containing a request to update an actor within the system.
 *
 * This class encapsulates the data required to request an update to a specific actor,
 * such as its unique identifier, updated properties, or state information.
 *
 * It is typically used in messaging systems where updates to actors need to be communicated
 * between different parts of the application or across network boundaries.
 *
 * Responsibilities of this class:
 * - Encapsulate information about the actor update request.
 * - Provide a structured way to relay actor update details.
 *
 * Use this class in conjunction with systems that require actor state synchronization
 * or have mechanisms that handle update messages to apply changes to actors.
 */
struct FActorUpdateRequestMessage : FActorUpdateBody
{
	/** The request owns the octets it sends, so the shared body's buffer is public on this side. */
	using FActorUpdateBody::StateBytes;

	/**
	 * Retrieves the type of the message.
	 *
	 * @return The message type as ECrowdyMessageType::ACTOR_UPDATE_REQUEST.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::ACTOR_UPDATE_REQUEST;
	}

	/**
	 * Retrieves the name of the message type.
	 *
	 * This method overrides the base class implementation to provide a unique name
	 * for the type of this message. It is used for identifying the message by its type name.
	 *
	 * @return An FName instance representing the name "Actor Update Request Message".
	 */
	virtual FName GetTypeName() const override
	{
		return "Actor Update Request Message";
	}

	/** The spatial header followed by the actor update body. */
	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata(BodySize());
		AppendBody(Data);
		return Data;
	}

	/** Send-only: an actor update arrives as FActorUpdateNotificationMessage, not as this. */
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("FActorUpdateRequestMessage::DecodePayload called, but should not be used."));
		return false;
	}

};
