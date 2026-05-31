#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * @class FVoxelUpdateNotificationMessage
 * @brief Represents a message structure used to notify about voxel updates.
 *
 * This class encapsulates the data and functionality required to handle
 * notifications related to voxel updates in the system. It is primarily used
 * for communication purposes, enabling different components of the system
 * to stay updated about voxel state changes.
 *
 * The notification may include information regarding what aspect of the
 * voxel data has been updated, allowing the receiving systems to act accordingly.
 */
struct FVoxelUpdateNotificationMessage : ICrowdyMessage
{
	
	
	int16 Vx, Vy, Vz;
	/**
	 * @brief Represents the type of a voxel in a voxel-based system.
	 *
	 * This variable stores a 16-bit signed integer that identifies the specific
	 * type or category of a voxel. Voxel types may correspond to different materials,
	 * objects, or states within a 3D environment. The exact meaning of each voxel
	 * type is typically determined by the application or system utilizing the voxel data.
	 */
	int16 VoxelType;
	/**
	 * @brief Represents the state of a voxel in the voxel-based system.
	 *
	 * This struct encapsulates various properties and attributes defining
	 * the state of a voxel, including its version, directional properties,
	 * rotation, override attributes, and associated game objects. It is used
	 * to describe the configuration or modifications applied to a voxel within
	 * the system.
	 *
	 * The `FVoxelState` variable is a central component in systems dealing with
	 * voxel manipulation and update notifications.
	 */
	TArray<uint8> StateBytes;
	uint16 StateSize;
	
	/**
	 * @brief Indicates whether the voxel update notification message contains voxel state data.
	 *
	 * This variable is set to true if the data being deserialized includes voxel state information.
	 * It is primarily used to determine whether additional state information has been provided in
	 * the serialized message and subsequently deserialized. By default, this value is set to false
	 * and only updated if the deserialization process detects and processes voxel state data.
	 */
	bool bContainsState = false;
	
	/**
	 * Retrieves the type of the current object or instance.
	 *
	 * This method is typically used to determine the runtime type of a given
	 * object, which can be useful for type identification, casting, or debugging
	 * purposes. The exact format of the returned type may depend on the implementation.
	 *
	 * @return A string representing the type of the current instance.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION;
	}

	/**
	 * Retrieves the name of the message type.
	 *
	 * This method returns the unique name associated with the voxel update notification message type.
	 * It provides a string identifier that can be useful for debugging, logging, or categorization purposes.
	 *
	 * @return An FName representing the name of the voxel update notification message type.
	 */
	virtual FName GetTypeName() const override
	{
		return "Voxel Update Notification Message";
	}

	/**
	 * Deserializes the provided byte array into the member variables of this class.
	 * The method extracts and assigns values for MapID, chunk coordinates, voxel coordinates,
	 * voxel type, and optionally the voxel state if the data length permits.
	 *
	 * @param Data The serialized byte array containing the voxel update data to deserialize.
	 */
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		const int32 DataLength = Data.Num();
		
		if (DataLength <= 0)
			return false;
		
		int32 Offset = 0;
		
		if (!DeserializeMetadata(Data, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FVoxelUpdateNotificationMessage::Deserialize - Metadata deserialization failed"));
			return false;
		}
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, Vx, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FVoxelUpdateNotificationMessage::Deserialize - Vx deserialization failed"));
			 return false;
		}
		Offset += sizeof(int16);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, Vy, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FVoxelUpdateNotificationMessage::Deserialize - Vy deserialization failed"));
			 return false;
		}
		Offset += sizeof(int16);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, Vz, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FVoxelUpdateNotificationMessage::Deserialize - Vz deserialization failed"));
			return false;
		}
		Offset += sizeof(int16);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, VoxelType, Offset))
		{
			UE_LOG(LogTemp, Warning, TEXT("FVoxelUpdateNotificationMessage::Deserialize - VoxelType deserialization failed"));
			return false;
		}
		
		Offset += sizeof(int16);
		
		if (Offset + sizeof(uint16) <= DataLength)
		{
			if (!USerializationFunctionLibrary::DeserializeValue(Data, StateSize, Offset))
			{
				UE_LOG(LogTemp, Warning, TEXT("FVoxelUpdateNotificationMessage::Deserialize - StateSize deserialization failed"));
				return false;
			}
			
			Offset += sizeof(uint16);
			
			if (StateSize > 0 && StateSize < 5000)
			{
				bContainsState = true;
				StateBytes.SetNumUninitialized(StateSize);
				FMemory::Memcpy(StateBytes.GetData(), Data.GetData() + Offset, StateSize);
			}
			
			return true;
		}
		
		UE_LOG(LogTemp, Warning, TEXT("FVoxelUpdateNotificationMessage::Deserialize - StateSize deserialization failed"));
		return false;
		
	}

	/**
	 * Serializes the provided object into a format suitable for storage or transmission,
	 * such as a string or binary.
	 *
	 * @param object The object to be serialized. Must not be null.
	 * @return A serialized representation of the input object.
	 *         Returns null if serialization fails.
	 */
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	/**
	 * Retrieves the size of the message in bytes.
	 *
	 * This method overrides the base implementation and provides
	 * the size of the message as a 32-bit unsigned integer.
	 *
	 * @return The size of the message in bytes, which is always 0 for this implementation.
	 */
	virtual uint32 GetMessageSize() const override
	{
		return 0;
	}
};

