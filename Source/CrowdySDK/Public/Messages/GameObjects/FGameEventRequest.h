#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * Represents a request for a specific ball event in a system,
 * which may contain details and parameters related to
 * processing or handling the event.
 */
struct FGameEventRequest : ICrowdyMessage
{
	
	uint16 EventType = 0;
	
	/**
	 * Represents the current status or condition of an object, system, or process.
	 */
	int32 StateSize;
	TArray<uint8> StateBytes;
	
	
	/**
	 * Retrieves the type of the current object or entity.
	 *
	 * @return The type of the object or entity as a string or equivalent representation.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
	}

	/**
	 * Retrieves the name of the type.
	 *
	 * @return The name of the type as a string.
	 */
	virtual FName GetTypeName() const override
	{
		return "Ball Event Activation Request";
	}

	/**
	 * Converts an object or data structure into a format suitable for storage or transmission.
	 *
	 * @return The serialized representation of the object or data structure.
	 */
	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata();
		
		Data.Append(USerializationFunctionLibrary::SerializeValue(EventType));
		Data.Append(USerializationFunctionLibrary::SerializeValue(StateSize));
		Data.Append(StateBytes);
		return Data;
	}

	/**
	 * Parses the provided data and reconstructs the original object or data structure.
	 *
	 * @param data The serialized input data to be deserialized.
	 * @param format The format or schema used during the serialization process, if applicable.
	 * @return The reconstructed object or data structure.
	 */
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		return false;
	}

	/**
	 * @brief Retrieves the total size of the message in bytes.
	 *
	 * This method calculates the size of the message by accounting for the sizes
	 * of its individual components. It is primarily used for message serialization
	 * and network communication purposes.
	 *
	 * Components included in the calculation:
	 * - Size of the MapID.
	 * - Size of three 64-bit integer parts: ChunkX, ChunkY, and ChunkZ.
	 * - Size of a fixed-length 32-byte string (ActivatorUUID in UTF-8 format).
	 * - Size of the EventType enum.
	 * - Size of the State object.
	 *
	 * @return The total size of the message in bytes, as a 32-bit unsigned integer.
	 */
	virtual uint32 GetMessageSize() const override
	{
		return sizeof(AppID) + sizeof(int64)*3 + 32 + sizeof(EventType) + StateSize;
	}

	
	
};
