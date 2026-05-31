#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Shared/Types/Structures/Events/FBaseEventState.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * @class FGameEventNotification
 * @brief A class designed to handle event-based notifications related to ball tracking or game states.
 *
 * This class provides mechanisms to manage and deliver notifications triggered by specific events
 * occurring within the gameplay or tracking systems. It encapsulates the necessary data and functionality
 * to support various event-handling scenarios in sports or simulation environments.
 *
 * The class is intended to be flexible, supporting different notification types, event listeners,
 * and data broadcasting to registered components or systems.
 *
 * Responsibilities of the FBallEventNotification class include:
 * - Storing event-specific details.
 * - Notifying registered observers or systems about the occurrence of events.
 * - Providing a structured way to handle ball state-related activities.
 *
 * Typical use cases:
 * - Updating game states when specific ball movements are detected.
 * - Triggering UI or audio feedback for in-game events.
 * - Integration with analytics or telemetry systems to log events.
 *
 * This class is designed as part of a larger system and may require interaction with other components
 * for full functionality.
 */
struct FGameEventNotification : ICrowdyMessage
{
	
	
	uint16 EventType;
	
	/**
	 * @brief Represents the state or condition of an entity or process.
	 *
	 * The State variable is used to describe and manage the current mode, status, or condition.
	 * It is typically part of a broader system and can take on different values depending
	 * on the context where it is applied. The exact behavior or usage is determined by its
	 * implementation or the system's requirements.
	 */
	int32 StateSize;
	TArray<uint8> StateBytes;
	FInstancedStruct State;
	
	/**
	 * Retrieves the type of the current object or instance.
	 *
	 * This method is typically used to determine the runtime type of an object
	 * and can be useful for type-checking or implementing type-based logic.
	 *
	 * @return The type of the current object or instance.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
	}

	/**
	 * Retrieves the name of the message type.
	 *
	 * This method returns an FName instance representing the unique identifier
	 * for the type of message. It is used to distinguish and identify the
	 * specific message type in the system.
	 *
	 * @return An FName instance containing the name of the message type as "Ball Event Notification".
	 */
	virtual FName GetTypeName() const override
	{
		return "Ball Event Notification";
	}

	/**
	 * Serializes the given object into a format suitable for storage or transmission.
	 *
	 * This method converts the object into a specific serialized format, such as JSON, XML,
	 * or binary data, depending on the implementation. The resulting serialized data
	 * can then be saved to a file, sent over a network, or used for other purposes.
	 *
	 * @return A string or byte array representing the serialized form of the object.
	 */
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	/**
	 * Deserializes a given input into an object of the specified type.
	 *
	 * This method converts serialized data, such as JSON or XML, back
	 * into the original object representation. It ensures the integrity
	 * of the deserialized data and throws exceptions if the process fails.
	 *
	 * @param input The serialized data to be deserialized.
	 * @param type The target class type to deserialize the input into.
	 * @throws IOException If there is an issue reading the input.
	 * @throws IllegalArgumentException If the input or type is invalid.
	 * @throws DeserializationException If the deserialization process fails.
	 */
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		int32 Offset = 0;
		
		if (!DeserializeMetadata(Data, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FGameEventNotification::Deserialize - Metadata deserialization failed"));
			return false;
		}
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, EventType, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FGameEventNotification::Deserialize - EventType deserialization failed"));
			return false;
		}
		
		Offset += sizeof(EventType);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, StateSize, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FGameEventNotification::Deserialize - StateSize deserialization failed"));
			return false;
		}
		Offset += sizeof(StateSize);
		
		// Safety check to prevent crashes
		constexpr int32 MAX_STATE_SIZE = 1024 * 1024; // 1 MB, adjust based on your system
		
		if (StateSize < 0 || StateSize > MAX_STATE_SIZE || Offset + StateSize > Data.Num())
		{
			UE_LOG(LogTemp, Warning, TEXT("FGameEventNotification::Deserialize - Invalid StateSize %d or Data overflow"), StateSize);
			StateBytes.Empty();
			return false; // safely skip deserialization
		}
		
		
		StateBytes.SetNumUninitialized(StateSize);
		FMemory::Memcpy(StateBytes.GetData(), Data.GetData() + Offset, StateSize);
		
		if (!USerializationFunctionLibrary::DeserializeEventState(StateBytes, State))
			State.Reset();
		
		return true;
	}

	/**
	 * Calculates the size of the message in bytes.
	 *
	 * This method overrides the base class implementation to return the size of
	 * the message, calculated as the sum of the sizes of its member variables.
	 * The returned size includes the sizes of the MapID, Chunk coordinates,
	 * UUID (fixed size of 32 bytes), EventType, and State variables.
	 *
	 * @return The total size of the message in bytes as a 32-bit unsigned integer.
	 */
	virtual uint32 GetMessageSize() const override
	{
		return sizeof(AppID) + sizeof(int64)*3 + 32 + sizeof(EventType) + StateSize;
	}

	
};
