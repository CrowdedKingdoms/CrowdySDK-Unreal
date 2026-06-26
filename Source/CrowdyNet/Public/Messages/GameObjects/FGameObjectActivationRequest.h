#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Shared/Types/Structures/GameObjects/FGameObjectState.h"
#include "Utils/SerializationFunctionLibrary.h"

/**

 * @brief Represents a request to activate or deactivate a game object within the system.
 *
 * The FGameObjectActivationRequest class encapsulates the data and logic needed to manage
 * activation and deactivation requests for game objects. This class is typically used in systems
 * where game objects are activated or deactivated dynamically based on game events, player interactions,
 * or other conditions.
 *
 * This class allows for precise control over the activation state of game objects and handles
 * optional metadata or parameters that may be required for specific activation scenarios.
 *
 * Responsibilities:
 * - Encapsulate the data needed to activate or deactivate a game object.
 * - Provide a structure to define the target game object and its activation state.
 * - Optionally allow the specification of additional parameters or conditions.
 */
struct FGameObjectActivationRequest : ICrowdyMessage
{
	
	/**
	 * @brief A unique identifier for a map instance.
	 *
	 * The MapID variable is used to distinguish between different map instances
	 * within an application. It serves as a primary key or reference for accessing
	 * specific map-related data or features.
	 *
	 * MapID values are typically assigned and managed by the system or application
	 * logic to ensure uniqueness.
	 */
	int64 MapID;
	/**
	 * @brief Represents the X-coordinate of a chunk in a grid or map structure.
	 *
	 * This variable is used to identify or access a specific chunk's position
	 * along the horizontal axis in a larger grid-based or tiled system.
	 * It typically corresponds to the chunk's column location.
	 *
	 * Use this variable in scenarios where grid-based partitioning or chunking
	 * of data is implemented, such as in game development or spatial data
	 * management systems.
	 */
	int64 ChunkX, ChunkY, ChunkZ;
	/**
	 * A unique identifier representing an activator entity.
	 *
	 * This variable is used to store a UUID (Universally Unique Identifier)
	 * that serves as a distinctive key for an activator. It ensures global
	 * uniqueness and can be used for identifying, retrieving, or
	 * referencing activator-related data in the system.
	 *
	 * The format of the UUID follows the standard 128-bit identifier,
	 * typically expressed as a string in the form "xxxxxxxx-xxxx-Mxxx-Nxxx-xxxxxxxxxxxx".
	 *
	 * Key Characteristics:
	 * - Ensures no two activators share the same identifier.
	 * - Facilitates reliable data lookup and handling in distributed systems.
	 *
	 * Usage Context:
	 * - Commonly employed in systems requiring consistent and unique
	 *   activator identification across various environments.
	 *
	 * Note:
	 * Proper handling should be ensured to maintain the immutability
	 * and integrity of the UUID value.
	 */
	FString ActivatorUUID;
	/**
	 * @brief Represents the type of an event in the system.
	 *
	 * This variable is used to classify or categorize events, enabling
	 * the system to handle different types of events accordingly. The
	 * specific values and their meanings depend on the context in which
	 * this variable is used.
	 *
	 * It is often employed in event-driven architectures and systems
	 * that depend on the identification of various event types for
	 * proper processing or routing.
	 */
	uint16 EventType;
	/**
	 * @brief Represents the state of an entity or a system within a specific context.
	 *
	 * The State variable is used to define or track the current condition or mode
	 * of operation that an entity or process is in. It can be used to manage
	 * transitions, maintain flow control, or represent logical steps in a stateful
	 * operation.
	 *
	 * Typical use cases include:
	 * - Managing the states of a finite state machine (FSM).
	 * - Defining operational modes in a system.
	 * - Tracking progression or conditions during execution.
	 */
	TArray<uint8> StateBytes;
	int32 StateSize;
	
	/**
	 * Retrieves the type of the current object or entity.
	 *
	 * @return A string representing the type of the object or entity.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
	}

	/**
	 * Retrieves the name of the message type specific to this activation request.
	 *
	 * This method is an override of the base ICrowdyMessage interface. It provides
	 * a unique identifier for the message type, used for distinguishing this
	 * message from other message types in the system.
	 *
	 * @return An FName instance containing the name "GameObject Activation Request"
	 * that represents this specific message type.
	 */
	virtual FName GetTypeName() const override
	{
		return "GameObject Activation Request";
	}

	/**
	 * Serializes the given object into a specific format such as JSON or XML.
	 *
	 * This method takes an input object and converts it into a string representation
	 * based on the desired serialization format. The generated output string
	 * can then be stored or transmitted as needed.
	 *
	 * @param object The object to be serialized. Must not be null.
	 * @return A string representation of the serialized object.
	 * @throws IllegalArgumentException if the input object is null.
	 * @throws SerializationException if the serialization process encounters an error.
	 */
	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data;
		Data.Reserve(GetMessageSize());
		
		Data.Add(static_cast<uint8>(GetType()) & 0xFF);
		
		Data.Append(USerializationFunctionLibrary::SerializeValue(MapID));
		
		Data.Append(USerializationFunctionLibrary::SerializeValue(ChunkX));
		Data.Append(USerializationFunctionLibrary::SerializeValue(ChunkY));
		Data.Append(USerializationFunctionLibrary::SerializeValue(ChunkZ));
		
		const FTCHARToUTF8 ConvertedUUID(*ActivatorUUID);
		Data.Append(reinterpret_cast<const uint8*>(ConvertedUUID.Get()), ConvertedUUID.Length());
		
		Data.Append(USerializationFunctionLibrary::SerializeValue(EventType));
		
		Data.Append(USerializationFunctionLibrary::SerializeValue(StateSize));
		Data.Append(StateBytes);
		
		return Data;
	}

	/**
	 * Parses and initializes the object's state using the provided binary data.
	 *
	 * This method overrides the base class's Deserialize function and is used to
	 * reconstruct the object's state from a serialized byte array.
	 *
	 * @param Data A reference to an array of bytes representing serialized data
	 *             that should be deserialized to initialize the object's state.
	 */
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		return;
	}

	/**
	 * Retrieves the size of a given message in bytes.
	 *
	 * This method calculates and returns the size of a message, which can be
	 * useful for serialization, networking, or storage purposes.
	 *
	 * @return The size of the message in bytes.
	 */
	virtual uint32 GetMessageSize() const override
	{
		return sizeof(MapID) + sizeof(int64)*3 + 32 + sizeof(EventType) + sizeof(State);
	}

	
};
