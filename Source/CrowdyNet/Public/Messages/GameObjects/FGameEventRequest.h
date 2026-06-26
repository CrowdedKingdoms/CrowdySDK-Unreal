#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * Wire message carrying a game event payload (an FInstancedStruct) from this
 * client to the relay server; send-only — deserialization is handled by
 * FGameEventNotification.
 *
 * Target/TargetID are appended after the payload so relays and old clients
 * that only understand the legacy layout pass them through / ignore them.
 */
struct FGameEventRequest : ICrowdyMessage
{
	uint16 EventType = 0;

	int32 StateSize = 0;
	TArray<uint8> StateBytes;

	ECrowdyTarget Target = ECrowdyTarget::Everyone;
	FGuid TargetID;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Game Event Request";
	}

	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata();

		Data.Append(USerializationFunctionLibrary::SerializeValue(EventType));
		Data.Append(USerializationFunctionLibrary::SerializeValue(StateSize));
		Data.Append(StateBytes);

		Data.Add(static_cast<uint8>(Target));

		// Same 32-hex-digit convention as the sender UUID in the metadata block.
		const FTCHARToUTF8 ConvertedTargetID(*TargetID.ToString(EGuidFormats::Digits));
		Data.Append(reinterpret_cast<const uint8*>(ConvertedTargetID.Get()), ConvertedTargetID.Length());

		return Data;
	}

	// Send-only message — nothing to deserialize.
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		return false;
	}

	// Metadata (AppID + chunk coords + 32-byte UUID) + EventType + state payload
	// + envelope (Target byte + 32-byte TargetID).
	virtual uint32 GetMessageSize() const override
	{
		return sizeof(AppID) + sizeof(int64)*3 + 32 + sizeof(EventType) + StateSize + sizeof(uint8) + 32;
	}
};
