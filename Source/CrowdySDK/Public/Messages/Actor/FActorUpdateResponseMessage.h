#pragma once
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * Encapsulates a response message for updating an actor.
 * This class is used to handle and process the results of an actor update operation.
 */
struct FActorUpdateResponseMessage : ICrowdyMessage
{
	
	ECrowdyErrorCode ErrorCode;
	
	/**
	 * Retrieves the specific type of the message.
	 *
	 * This method overrides the base class implementation to return the specific message
	 * type associated with the FActorUpdateResponseMessage. This type is used to identify
	 * the message as an actor update response in the ECrowdyMessageType enumeration.
	 *
	 * @return ECrowdyMessageType::ACTOR_UPDATE_RESPONSE The type of the message as
	 * an enumerator from ECrowdyMessageType.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::ACTOR_UPDATE_RESPONSE;
	}

	/**
	 * Retrieves the name of the message type.
	 *
	 * This method overrides the base class implementation to return a unique
	 * identifier for the message type as an FName. The returned name is
	 * specific to the "Actor Update Response Message" type.
	 *
	 * @return An FName instance representing the name of the message type.
	 */
	virtual FName GetTypeName() const override
	{
		return "Actor Update Response Message";
	}

	/**
	 * Deserializes the provided data into its original structure or object format.
	 *
	 * @param Data
	 * @param data The serialized data to be deserialized.
	 * @param format The format in which the data is serialized, such as JSON or XML.
	 */
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		int32 Offset = 0;
		
		if (Data.Num() <= 0)
			return false;
		
		SequenceNumber = static_cast<uint8>(Data[Offset]);
		Offset += sizeof(uint8);
		ErrorCode = static_cast<ECrowdyErrorCode>(Data[Offset]);
		
		return true;
	}

	/**
	 * Converts an object or data structure into a format suitable for storage or transmission.
	 * The serialized output can later be deserialized to reconstruct the original object.
	 *
	 * @return A serialized representation of the object or data structure.
	 */
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	/**
	 * Calculates and returns the size of the message in bytes.
	 *
	 * This method provides the total size of the message, which is determined
	 * by summing the sizes of its individual components. The size includes the
	 * memory required for storing map information, chunk coordinates, actor
	 * identifier, and additional metadata. It can be used to determine the
	 * message's footprint during serialization or transmission processes.
	 *
	 * @return The size of the message in bytes as a 32-bit unsigned integer.
	 */
	virtual uint32 GetMessageSize() const override { return sizeof(int64) * 4 + 32 + 1; }

	
};
