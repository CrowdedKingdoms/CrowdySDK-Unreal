#pragma once
#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Shared/Types/Structures/Events/FBaseEventState.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * Wire message carrying a game event payload (an FInstancedStruct) from the relay
 * server to clients; receive-only — serialization is handled elsewhere.
 */
struct FGameEventNotification : ICrowdyMessage
{
	uint16 EventType;

	int32 StateSize;
	TArray<uint8> StateBytes;
	FInstancedStruct State;

	// Envelope appended after the payload by FGameEventRequest. Messages from
	// clients running pre-envelope code simply lack these bytes, so the
	// defaults preserve legacy broadcast behavior.
	ECrowdyTarget Target = ECrowdyTarget::Everyone;
	FGuid TargetID;

	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
	}

	virtual FName GetTypeName() const override
	{
		return "Game Event Notification";
	}

	// Receive-only message — nothing to serialize.
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		int32 Offset = 0;

		if (!DeserializeMetadata(Data, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FGameEventNotification::Deserialize - Metadata deserialization failed"));
			return false;
		}

		if (!USerializationFunctionLibrary::DeserializeValue(Data, EventType, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FGameEventNotification::Deserialize - EventType deserialization failed"));
			return false;
		}

		Offset += sizeof(EventType);

		if (!USerializationFunctionLibrary::DeserializeValue(Data, StateSize, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FGameEventNotification::Deserialize - StateSize deserialization failed"));
			return false;
		}
		Offset += sizeof(StateSize);

		// Safety check to prevent crashes
		constexpr int32 MAX_STATE_SIZE = 1024 * 1024; // 1 MB, adjust based on your system

		if (StateSize < 0 || StateSize > MAX_STATE_SIZE || Offset + StateSize > Data.Num())
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FGameEventNotification::Deserialize - Invalid StateSize %d or Data overflow"), StateSize);
			StateBytes.Empty();
			return false; // safely skip deserialization
		}


		StateBytes.SetNumUninitialized(StateSize);
		FMemory::Memcpy(StateBytes.GetData(), Data.GetData() + Offset, StateSize);

		if (!USerializationFunctionLibrary::DeserializeEventState(StateBytes, State))
			State.Reset();

		Offset += StateSize;

		// Target byte + 32-hex-digit TargetID sit between the payload and the
		// relay-appended tail. Length-checked so pre-envelope senders still parse.
		constexpr int32 EnvelopeSize = sizeof(uint8) + 32;
		if (Data.Num() - TailSize - Offset >= EnvelopeSize)
		{
			const uint8 RawTarget = Data[Offset];
			Target = RawTarget <= static_cast<uint8>(ECrowdyTarget::AllExceptSender)
				? static_cast<ECrowdyTarget>(RawTarget)
				: ECrowdyTarget::Everyone;
			Offset += sizeof(uint8);

			TargetID = USerializationFunctionLibrary::ToGuid(
				USerializationFunctionLibrary::DeserializeString(Data, Offset, 32));
		}

		return true;
	}

	// Metadata (AppID + chunk coords + 32-byte UUID) + EventType + state payload
	// + envelope (Target byte + 32-byte TargetID).
	virtual uint32 GetMessageSize() const override
	{
		return sizeof(AppID) + sizeof(int64)*3 + 32 + sizeof(EventType) + StateSize + sizeof(uint8) + 32;
	}


};
