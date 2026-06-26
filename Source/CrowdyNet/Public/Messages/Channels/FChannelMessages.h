#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * Client -> server publish to a channel. Non-spatial: the server fans it out to every active
 * member of the channel except the sender, regardless of location.
 *
 * The frame is shorter than a spatial message and uses a different layout, so it builds its own
 * bytes instead of SerializeMetadata(). It serializes through the containsAuth flag; the
 * hmac(32) + gameTokenId(8) + seq(1) trailer is appended by FCrowdyTransmissionLayerUDP::SendBytes
 * (bRequiresAuth = true), which is exactly the trailer this message type expects.
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
		Data.Add(static_cast<uint8>(GetType()));
		Data.Append(USerializationFunctionLibrary::SerializeValue(ChannelId));

		// 32 hex digits, no null terminator — same convention as the spatial UUID.
		const FTCHARToUTF8 ConvertedUUID(*UUID);
		Data.Append(reinterpret_cast<const uint8*>(ConvertedUUID.Get()), ConvertedUUID.Length());

		const uint16 PayloadLength = static_cast<uint16>(Payload.Num());
		Data.Append(USerializationFunctionLibrary::SerializeValue(PayloadLength));
		Data.Append(Payload);

		Data.Add(static_cast<uint8>(1)); // containsAuth — always signed
		return Data;
	}

	// Send-only message — the server delivers it as an FChannelMessageNotification.
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		return false;
	}

	virtual uint32 GetMessageSize() const override
	{
		return sizeof(ChannelId) + 32 + sizeof(uint16) + Payload.Num() + sizeof(uint8);
	}
};

/**
 * Server -> client delivery of a channel message. Not HMAC-signed (the payload is untrusted —
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

	// Receives the datagram with the leading type byte already stripped by FCrowdyMessageParser.
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		constexpr int32 HeaderSize = sizeof(int64) + 32 + sizeof(uint16);
		if (Data.Num() < HeaderSize)
			return false;

		int32 Offset = 0;

		if (!USerializationFunctionLibrary::DeserializeValue(Data, ChannelId, Offset))
			return false;
		Offset += sizeof(int64);

		UUID = USerializationFunctionLibrary::DeserializeString(Data, Offset, 32);
		if (UUID.Len() != 32)
			return false;
		Offset += 32;

		uint16 PayloadLength = 0;
		if (!USerializationFunctionLibrary::DeserializeValue(Data, PayloadLength, Offset))
			return false;
		Offset += sizeof(uint16);

		if (Data.Num() < Offset + PayloadLength)
			return false;

		Payload.SetNumUninitialized(PayloadLength);
		FMemory::Memcpy(Payload.GetData(), Data.GetData() + Offset, PayloadLength);
		Offset += PayloadLength;

		// Tail: server timestamp then the sender's sequence number, both optional on the wire.
		if (Data.Num() >= Offset + static_cast<int32>(sizeof(int64)) + 1)
		{
			USerializationFunctionLibrary::DeserializeValue(Data, Timestamp, Offset);
			Offset += sizeof(int64);
			SequenceNumber = Data[Offset];
		}

		return true;
	}

	virtual uint32 GetMessageSize() const override
	{
		return sizeof(ChannelId) + 32 + sizeof(uint16) + Payload.Num() + sizeof(int64) + sizeof(uint8);
	}
};
