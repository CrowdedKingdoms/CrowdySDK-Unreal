#pragma once
#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Messages/GameObjects/FGameEventBody.h"
#include "Serialization/CrowdyActorId.h"
#include "Serialization/CrowdyFrame.h"
#include "Shared/Types/Structures/Events/FBaseEventState.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * Wire message carrying a game event payload (an FInstancedStruct) from the relay
 * server to clients; receive-only, since serialization is handled elsewhere.
 */
struct FGameEventNotification : FGameEventBody
{
	FInstancedStruct State;

	/**
	 * The payload octets exactly as they arrived, borrowed from the frame rather than copied out of it.
	 *
	 * Valid only for the duration of the delivery call this message is passed to. Read it, or decode what
	 * you need out of it, before returning; a handler that hands work to a later frame must capture the
	 * DECODED value, never this view or the message that carries it. The decoded State above is owned and
	 * has no such limit.
	 */
	TConstArrayView<uint8> StateView;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Game Event Notification";
	}

	// The event type number is the routing key for every carrier of this layout: the client event,
	// the server event and the single-actor send all inherit this and need no special case.
	virtual FCrowdyPayloadKey GetPayloadKey() const override
	{
		return FCrowdyPayloadKey::Event(EventType);
	}

	virtual const FInstancedStruct* GetPayload() const override
	{
		return State.IsValid() ? &State : nullptr;
	}

	virtual TConstArrayView<uint8> GetPayloadBytes() const override
	{
		return StateView;
	}

	// Receive-only message: nothing to serialize.
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		const TConstArrayView<uint8> Data = Frame.Body;
		int32 Offset = 0;

		ApplyEnvelope(Frame);
		if (!ApplyEnvelopeActorId(Frame))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FGameEventNotification::DecodePayload - the frame carries no actor id"));
			return false;
		}

		if (!USerializationFunctionLibrary::DeserializeValue(Data, EventType, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FGameEventNotification::DecodePayload - EventType deserialization failed"));
			return false;
		}

		Offset += sizeof(EventType);

		if (!USerializationFunctionLibrary::DeserializeValue(Data, StateSize, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FGameEventNotification::DecodePayload - StateSize deserialization failed"));
			return false;
		}
		Offset += sizeof(StateSize);

		// Safety check to prevent crashes
		constexpr int32 MAX_STATE_SIZE = 1024 * 1024; // 1 MB, adjust based on your system

		if (StateSize < 0 || StateSize > MAX_STATE_SIZE || Offset + StateSize > Data.Num())
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FGameEventNotification::DecodePayload - Invalid StateSize %d or Data overflow"), StateSize);
			StateView = TConstArrayView<uint8>();
			return false; // safely skip deserialization
		}

		// The bound above is what keeps this view inside the frame, so it is load-bearing rather than
		// defensive: nothing copies the octets out, and every reader addresses them through this.
		StateView = TConstArrayView<uint8>(Data.GetData() + Offset, StateSize);

		if (!USerializationFunctionLibrary::DeserializeEventState(StateView, State))
			State.Reset();

		Offset += StateSize;

		// Target byte + a 32 octet target id sit after the payload, at the end of this message's own
		// bytes. Length-checked so senders running pre-envelope code still parse.
		constexpr int32 TargetBlockSize = sizeof(uint8) + FCrowdyActorId::NumOctets;
		if (Data.Num() - Offset >= TargetBlockSize)
		{
			const uint8 RawTarget = Data[Offset];
			Target = RawTarget <= static_cast<uint8>(ECrowdyTarget::AllExceptSender)
				? static_cast<ECrowdyTarget>(RawTarget)
				: ECrowdyTarget::Everyone;
			Offset += sizeof(uint8);

			// Read as octets rather than as text. Reading the same 32 octets into a string and converting
			// back re-encodes anything outside ASCII to a different length, so the two paths would derive
			// two different keys from one set of bytes.
			const FCrowdyActorId TargetOctets = FCrowdyActorId::FromOctets(
				TConstArrayView<uint8>(Data.GetData() + Offset, FCrowdyActorId::NumOctets));
			TargetID = USerializationFunctionLibrary::ToGuid(TargetOctets);
		}

		return true;
	}

};
