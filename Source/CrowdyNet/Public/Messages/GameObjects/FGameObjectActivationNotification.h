#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Shared/Types/Structures/GameObjects/FGameObjectState.h"
#include "Utils/SerializationFunctionLibrary.h"

/**

 * @brief Encapsulates activation and deactivation notifications for a game object.
 *
 * The FGameObjectActivationNotification class is responsible for tracking and
 * managing the notification states of a game object upon its activation or
 * deactivation. This class is typically used in systems where game object lifecycle
 * events need to trigger specific responses or be monitored for various gameplay
 * or system purposes.
 *
 * This class provides mechanisms to handle and propagate changes in the activation
 * state of the associated game object. It can be extended or integrated within
 * game systems that require controlled handling of activation events.
 */
struct FGameObjectActivationNotification : ICrowdyMessage
{
	
	/**
	 * @brief Unique identifier for the map associated with the message.
	 *
	 * This 64-bit integer represents the unique ID of a map in the system.
	 * It is used in serialization and deserialization processes to specify the
	 * map to which the associated message or event corresponds. The value is
	 * manipulated and transmitted as part of the FGameObjectActivationNotification
	 * message structure.
	 *
	 * The MapID plays a key role in identifying the context and scope of a
	 * particular message or event involving game world objects.
	 */
	int64 MapID;
	/**
	 * Represents the X-coordinate of a chunk in a three-dimensional grid system.
	 *
	 * The value is used within the context of spatial partitioning to identify
	 * and locate chunks in the X-axis. This coordinate, along with ChunkY and ChunkZ,
	 * defines a unique position in the 3D grid.
	 */
	int64 ChunkX, ChunkY, ChunkZ;
	/**
	 * A unique identifier representing the activator of a game object event.
	 *
	 * This string holds a 32-character UUID value used to uniquely identify the
	 * entity or system that triggered a specific game object activation event.
	 * It is typically deserialized from binary data when reconstructing a
	 * message or event state.
	 */
	FString ActivatorUUID;
	/**
	 * @brief Represents the type of an event in the context of game object activation notifications.
	 *
	 * This variable is used to store a 16-bit integer identifier that categorizes
	 * the specific type of event associated with a `FGameObjectActivationNotification` message.
	 * It aids in distinguishing between different event types during serialization,
	 * deserialization, and message handling within the system.
	 *
	 * @remarks Typically utilized in scenarios involving event-based communication
	 * in distributed systems or game logic to signal a particular activation event.
	 */
	uint16 EventType;
	/**
	 * @brief Represents the state or status of an object, process, or system.
	 *
	 * The `State` variable is used to encapsulate the current condition, mode,
	 * or stage of an element within a system. It can be used to manage transitions,
	 * perform logic based on specific states, or track progress over time.
	 *
	 * Typically, the variable can represent various predefined states or conditions,
	 * such as active, inactive, pending, completed, or custom-defined states based on use cases.
	 *
	 * Proper handling of the `State` variable ensures the system operates as intended
	 * and provides robust control over workflows and processes.
	 */
	int32 StateSize;
	TArray<uint8> StateBytes;
	
	/**
	 * Retrieves the type of the object as a string representation.
	 *
	 * This method is typically used to determine the runtime type
	 * of an object or class for reflection or type-checking purposes.
	 *
	 * @return A string representing the type of the object.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
	}

	/**
	 * Retrieves the name of the specified type.
	 *
	 * This method takes a type object and returns its corresponding name
	 * as a string representation. It can be used to obtain the simple name
	 * or fully qualified name of a type depending on implementation.
	 *
	 * @return the name of the type as a string
	 */
	virtual FName GetTypeName() const override
	{
		return "GameObject Activation Notification";
	}

	/**
	 * Serializes the given data structure or object into a string format.
	 * The result can be used for storage, transmission, or further processing.
	 *
	 * @return A string representation of the serialized data.
	 */
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	/**
	 * Parses and initializes the state of the object using the provided serialized data.
	 * This method reads the binary data and updates the member variables of the class
	 * to reconstruct the state of the object from the serialized data.
	 *
	 * @param Data A reference to a TArray of uint8 containing the serialized data
	 *             to be deserialized.
	 */
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		if (Data.Num() <= 0)
			return false;
		
		int32 Offset = 0;
		
		USerializationFunctionLibrary::DeserializeValue(Data, MapID, Offset);
		Offset += sizeof(MapID);
		
		USerializationFunctionLibrary::DeserializeValue(Data, ChunkX, Offset);
		Offset += sizeof(ChunkX);
		USerializationFunctionLibrary::DeserializeValue(Data, ChunkY, Offset);
		Offset += sizeof(ChunkY);
		USerializationFunctionLibrary::DeserializeValue(Data, ChunkZ, Offset);
		Offset += sizeof(ChunkZ);
		
		ActivatorUUID = USerializationFunctionLibrary::DeserializeString(Data, Offset, 32);
		Offset += 32;
		
		USerializationFunctionLibrary::DeserializeValue(Data, EventType, Offset);
		Offset += sizeof(EventType);
		
		USerializationFunctionLibrary::DeserializeValue(Data, StateSize , Offset);
		Offset += sizeof(StateSize);
		
		FMemory::Memcpy(StateBytes.GetData(), Data.GetData() + Offset, StateSize);
		
		return true;
	}

	/**
	 * Calculates and retrieves the total size of the message in bytes.
	 *
	 * This method determines the size of the object by summing the sizes of its components,
	 * including MapID, 3 int64 values (ChunkX, ChunkY, ChunkZ), a 32-character string (ActivatorUUID),
	 * EventType, and State. It is useful for serialization or network transmission purposes.
	 * Overrides the base class implementation to provide the specific message size.
	 *
	 * @return The size of the message in bytes as a 32-bit unsigned integer.
	 */
	virtual uint32 GetMessageSize() const override
	{
		return sizeof(MapID) + sizeof(int64)*3 + 32 + sizeof(EventType) + StateSize;
	}

	
};
