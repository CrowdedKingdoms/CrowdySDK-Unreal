#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"

/**
 * @class FDefaultMessage
 * @brief Represents a default message structure in the system.
 *
 * This class is designed to manage and encapsulate the functionality
 * of a standard message object. It provides a framework to define, store,
 * and manipulate message data in a consistent format throughout the system.
 *
 * The FDefaultMessage class is useful for handling basic message operations,
 * ensuring compatibility and consistency when interacting with related components
 * or subsystems.
 *
 * Typical functionalities of this class include setting data, retrieving data,
 * and other operations specific to message handling. The class serves as a base
 * structure on which more specialized or extended message functionalities can be built.
 */
struct FDefaultMessage : ICrowdyMessage
{
	/**
	 * Retrieves the type of the message.
	 *
	 * This override implementation always returns ECrowdyMessageType::BAD_MESSAGE,
	 * indicating that the message type is invalid, unrecognized, or a default placeholder.
	 *
	 * @return ECrowdyMessageType::BAD_MESSAGE, representing the type of the message.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::BAD_MESSAGE;
	}

	/**
	 * Retrieves the name of the message type.
	 *
	 * This method overrides the GetTypeName function from the ICrowdyMessage interface.
	 * It returns a default or unknown message name as an FName object, representing
	 * the type of the message. This is useful when a specific type name is not defined
	 * for the message.
	 *
	 * @return An FName instance containing the string "Unknown or Default Message",
	 * which represents the default or unknown name of the message type.
	 */
	virtual FName GetTypeName() const override
	{
		return "Unknown or Default Message";
	}

	/**
	 * Serializes the message into a byte array.
	 *
	 * This method overrides the base class implementation by providing
	 * specific serialization behavior for the FDefaultMessage structure.
	 * In this implementation, an empty array is returned.
	 *
	 * @return A TArray of uint8 containing the serialized representation
	 * of the message. For FDefaultMessage, this will always be an empty array.
	 */
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	/**
	 * Deserializes the provided binary data and updates the object's state.
	 *
	 * This method overrides the Deserialize function in the base ICrowdyMessage interface.
	 * It takes serialized binary data as input and processes it to update the object's state.
	 *
	 * @param Data A reference to an array of bytes representing the serialized input data
	 *             that should be deserialized.
	 */
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		return false;
	}

	/**
	 * Retrieves the size of the message in bytes.
	 *
	 * This implementation always returns 0, indicating that the default message
	 * has no meaningful size.
	 *
	 * @return The size of the message in bytes as a 32-bit unsigned integer, which is always 0.
	 */
	virtual uint32 GetMessageSize() const override
	{
		return 0;
	}
	
	
};