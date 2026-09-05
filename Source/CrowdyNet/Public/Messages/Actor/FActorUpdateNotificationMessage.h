#pragma once
#include "CrowdyNetLog.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Subscription/FCrowdyPayloadKey.h"
#include "Messages/Actor/FActorUpdateBody.h"
#include "Serialization/CrowdyFrame.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * Represents a message that notifies subscribers about updates related to an actor.
 *
 * This class is designed to encapsulate all relevant information required to
 * convey the details of an actor's state update. It can include metadata or
 * information such as updated properties, timestamps, or any other data
 * necessary for consumers to process the notification.
 */
struct FActorUpdateNotificationMessage : FActorUpdateBody
{

	FGuid GUID;
	int32 ExpectedStateSize = 300;
	FInstancedStruct State;

	/**
	 * The state octets exactly as they arrived, borrowed from the frame rather than copied out of it.
	 *
	 * Valid only for the duration of the delivery call this message is passed to. Read it, or decode what
	 * you need out of it, before returning; a handler that hands work to a later frame must capture the
	 * DECODED value, never this view or the message that carries it. The decoded State above is owned and
	 * has no such limit.
	 */
	TConstArrayView<uint8> StateView;

	/**
	 * The payload type tag that leads the state blob. Kept even when no struct is registered for it,
	 * so the message can still say what arrived instead of losing all knowledge of itself.
	 */
	FCrowdyTypeID PayloadTypeID = CROWDY_INVALID_TYPE_ID;

	/**
	 * The entity class the sender named. This is the message's answer to "what is this", where
	 * PayloadTypeID above answers "what shape are these bytes"; conflating the two is what let any two
	 * classes sharing a state struct resolve as whichever of them registered last.
	 *
	 * Only ever set from a frame that decoded, since the decoder refuses a frame naming no class.
	 */
	FCrowdyClassID PayloadClassID = CROWDY_INVALID_CLASS_ID;

	/**
	 * Retrieves the specific type of the message.
	 *
	 * This method overrides the pure virtual function defined in the ICrowdyMessage interface.
	 * It is used to identify the type of the message, which in this case is an actor update notification.
	 *
	 * @return ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION, indicating that this message type
	 * represents an actor update notification.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION;
	}

	/**
	 * Retrieves the name of the message type.
	 *
	 * This method returns an FName that represents the unique name of the message type
	 * associated with this class. It is used to identify and distinguish the message type
	 * in a human-readable format.
	 *
	 * @return An FName instance containing the string "Actor Update Notification Message".
	 */
	virtual FName GetTypeName() const override
	{
		return "Actor Update Notification Message";
	}

	virtual FCrowdyPayloadKey GetPayloadKey() const override
	{
		return FCrowdyPayloadKey::ActorUpdate(PayloadTypeID);
	}

	virtual const FInstancedStruct* GetPayload() const override
	{
		return State.IsValid() ? &State : nullptr;
	}

	virtual TConstArrayView<uint8> GetPayloadBytes() const override
	{
		return StateView;
	}

	/**
	 * Decodes the given frame into its corresponding object representation.
	 *
	 * This method takes the frame's payload and reconstructs the original
	 * object by parsing the data and mapping it to the appropriate fields.
	 *
	 * @param Frame The frame carrying the serialized data to be decoded.
	 */
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		// Sits between Crowdy_DecodeFrame above it and Crowdy_ToGuid and Crowdy_DeserializeActorState
		// below it, so subtracting those two from this leaves the envelope parse and the framing reads,
		// neither of which can be scoped without splitting a helper that is not worth splitting yet.
		TRACE_CPUPROFILER_EVENT_SCOPE(Crowdy_DecodePayload_ActorUpdate);

		const TConstArrayView<uint8> Data = Frame.Body;
		int32 Offset = 0;

		ApplyEnvelope(Frame);
		if (!ApplyEnvelopeActorId(Frame))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FActorUpdateNotificationMessage::DecodePayload - the frame carries no actor id"));
			return false;
		}

		GUID = USerializationFunctionLibrary::ToGuid(UUID);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, StateSize, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FActorUpdateNotificationMessage::DecodePayload - StateSize deserialization failed"));
			return false;
		}
		
		Offset += sizeof(StateSize);
		
		if (StateSize <= 0)
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FActorUpdateNotificationMessage::DecodePayload - StateSize is out of valid range %d expected %d."),
			StateSize, ExpectedStateSize);
			return false;
		}
		
		// The declared length comes off the wire, so the bound is written as a comparison against the
		// bytes remaining. Adding it to the offset first would overflow for a large declared length and
		// produce a negative sum that passes the check. A malformed frame is expected input here rather
		// than a programming error, so it reports rather than ensuring.
		if (StateSize > Data.Num() - Offset)
		{
			UE_LOG(LogCrowdyNet, Warning,
				TEXT("FActorUpdateNotificationMessage::DecodePayload - declared state length %d runs past the end of a %d byte frame at offset %d."),
				StateSize, Data.Num(), Offset);
			return false;
		}
		
		// The bound above is what keeps this view inside the frame, so it is load-bearing rather than
		// defensive: nothing copies the octets out, and every reader addresses them through this.
		StateView = TConstArrayView<uint8>(Data.GetData() + Offset, StateSize);

		if (!USerializationFunctionLibrary::DeserializeActorState(StateView, State, PayloadTypeID, PayloadClassID))
			State.Reset();
		
		return true;
	}

	/** Receive-only: an actor update is sent as FActorUpdateRequestMessage, not as this. */
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	void SetExpectedStateSize(const int32 Size) { ExpectedStateSize = Size; }
	
};
