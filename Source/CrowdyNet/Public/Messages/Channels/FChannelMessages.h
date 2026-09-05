#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyFrame.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * Client -> server publish to a channel. Non-spatial: the server fans it out to every active
 * member of the channel except the sender, regardless of location.
 *
 * The frame is shorter than a spatial message and uses a different layout, so it builds its own
 * bytes instead of SerializeMetadata(). It serializes through the containsAuth flag; the
 * hmac(32) + gameTokenId(8) + seq(1) trailer is appended by the transport when it signs the frame,
 * which is exactly the trailer this message type expects.
 *
 *   [17][channelId(8)][uuid(32)][payloadLen(2)][payload][containsAuth=1]
 */
struct FChannelMessageRequest : ICrowdyMessage
{
	int64 ChannelId = 0;
	TArray<uint8> Payload;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CHANNEL_MESSAGE_REQUEST;
	}

	virtual FName GetTypeName() const override
	{
		return "Channel Message Request";
	}

	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data;

		// Sized for the whole frame before anything is written, so appending the payload cannot
		// reallocate and re-copy what is already there. The layout is fixed and short, so this is the
		// literal sum of it rather than a helper.
		Data.Reserve(sizeof(uint8) + sizeof(ChannelId) + FCrowdyActorId::NumOctets
			+ sizeof(uint16) + Payload.Num() + sizeof(uint8));

		Data.Add(static_cast<uint8>(GetType()));
		USerializationFunctionLibrary::AppendValue(Data, ChannelId);

		// The sender id occupies a fixed 32 octets with no length and no terminator, same as the spatial
		// header, so the payload length that follows it is always found at the same offset.
		UUID.AppendTo(Data);

		const uint16 PayloadLength = static_cast<uint16>(Payload.Num());
		USerializationFunctionLibrary::AppendValue(Data, PayloadLength);
		Data.Append(Payload);

		Data.Add(static_cast<uint8>(1)); // containsAuth - always signed
		return Data;
	}

	// Send-only message - the server delivers it as an FChannelMessageNotification.
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		return false;
	}

};

/**
 * Server -> client delivery of a channel message. Not HMAC-signed (the payload is untrusted -
 * validate it before acting on it). May arrive standalone or inside a MESSAGE_BUNDLE.
 *
 *   [18][channelId(8)][uuid(32)][payloadLen(2)][payload][epochMillis(8)][seq(1)]
 */
struct FChannelMessageNotification : ICrowdyMessage
{
	int64 ChannelId = 0;
	TArray<uint8> Payload;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Channel Message Notification";
	}

	virtual TArray<uint8> Serialize() const override
	{
		return {};
	}

	// The channel header, the declared payload length and the optional tail have all been read off the
	// frame already, so what is left is the payload itself.
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		ChannelId = Frame.Envelope.ChannelId;
		Timestamp = Frame.Envelope.Timestamp;
		SequenceNumber = Frame.Envelope.Sequence;

		// The sender id occupies a fixed 32 octets whatever it holds, but a server-native notification (a
		// model-driven re-pull hint, or any automation run with no acting client) has no sender, so the
		// region arrives empty or binary rather than as a 32-character hex string. Receivers act on
		// ChannelId and Payload, so it is taken for what it is worth and never rejected on: a strict
		// 32-character check once dropped every server-driven channel notification silently.
		UUID = FCrowdyActorId::FromOctets(Frame.Envelope.Uuid);

		Payload.SetNumUninitialized(Frame.Body.Num());
		if (Frame.Body.Num() > 0)
		{
			FMemory::Memcpy(Payload.GetData(), Frame.Body.GetData(), Frame.Body.Num());
		}

		return true;
	}

};
