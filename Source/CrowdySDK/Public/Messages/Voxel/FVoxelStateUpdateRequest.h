#pragma once

#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Shared/Types/Structures/Voxels/FVoxelState.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * @class FVoxelStateUpdateRequest
 * @brief Represents a request to update the state of a voxel.
 *
 * This class encapsulates all the information required to perform a voxel state update,
 * such as relevant properties, target updates, and processing data necessary for the operation.
 * It is typically used in voxel-based systems for runtime modifications of voxel properties.
 *
 * The state update request may include details for multi-threaded processing
 * and conflict resolution mechanisms to ensure data integrity.
 */
struct FVoxelStateUpdateRequest : ICrowdyMessage
{
	
	
	/**
	 * @brief Represents the Voxel Coordinates within a specific chunk
	 */
	int16 Vx, Vy, Vz;
	/**
	 * @brief Represents the type or category of a voxel in a voxel-based system.
	 *
	 * This variable is typically used to classify voxels based on their attributes, such as material,
	 * purpose, or behavior within a 3D grid or volumetric space. It aids in distinguishing different
	 * voxel properties and handling them appropriately in algorithms or rendering processes.
	 *
	 * The specific implementation or usage of VoxelType may vary depending on the voxel system and
	 * its requirements.
	 */
	int16 VoxelType;
	/**
	 * Represents the state of a voxel within the system, encapsulating various attributes
	 * such as its version, orientation, edit properties, and associated game objects.
	 *
	 * This structure is used to define and manipulate the state of a voxel and its associated
	 * metadata, providing support for serialization and interaction with the game environment.
	 * It includes properties for rotational states, atlas overrides, voxel-level-of-detail (VLO),
	 * voxel edit shapes, and sizes, as well as game objects placed within the voxel.
	 */
	
	TArray<uint8> StateBytes;
	uint16 StateSize = 0;
	
	
	bool bContainsState = false;
	
	
	/**
	 * Retrieves the type of the current object or instance.
	 *
	 * @return The type of the current object or instance as a string or type descriptor, depending
	 *         on the implementation and specific use case.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::VOXEL_UPDATE_REQUEST;
	}

	/**
	 * Retrieves the name of the message type for this instance.
	 *
	 * This overridden method returns the unique name identifier for the "Voxel State Update Request" message type.
	 *
	 * @return An FName instance representing the name of the "Voxel State Update Request" message type.
	 */
	virtual FName GetTypeName() const override
	{
		return "Voxel State Update Request";
	}

	/**
	 * Serializes the given object into a string representation.
	 *
	 * This method converts the object provided as input into a format
	 * that can be stored or transmitted and later reconstructed into
	 * the original object. The format of the serialized string depends
	 * on the implementation.
	 *
	 * @param object The object to be serialized. Must not be null.
	 * @return The string representation of the serialized object.
	 *         If the object cannot be serialized, returns null.
	 * @throws IllegalArgumentException if the object is of an unsupported type.
	 */
	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata();
		
		Data.Append(USerializationFunctionLibrary::SerializeValue(Vx));
		Data.Append(USerializationFunctionLibrary::SerializeValue(Vy));
		Data.Append(USerializationFunctionLibrary::SerializeValue(Vz));
		Data.Append(USerializationFunctionLibrary::SerializeValue(VoxelType));
		Data.Append(USerializationFunctionLibrary::SerializeValue(StateSize));
		
		if (bContainsState && StateBytes.Num() > 0)
		{
			Data.Append(StateBytes);
			UE_LOG(LogTemp, Log, TEXT("State Appended in message. State Size %d"), StateBytes.Num());
		}
		
		return Data;
	}
	

	/**
	 * Calculates and returns the size of a message in bytes.
	 *
	 * This method determines the total size of a given message, including all
	 * its associated components or fields, based on predefined rules or structure.
	 *
	 * @return The size of the message in bytes as an integer.
	 */
	virtual uint32 GetMessageSize() const override
	{
		return 1 + sizeof(int64) * 3 + sizeof(int16) * 4 + sizeof(FVoxelState);
	}

	/**
	 * Deserializes the given input data and reconstructs an object of the specified type.
	 *
	 * This method parses the serialized data provided as input and converts it
	 * back into an instance of the appropriate object or data structure.
	 *
	 * @param Data The serialized data to be deserialized, typically in the form of a string or byte array.
	 * 
	 */
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		UE_LOG(LogTemp, Warning, TEXT("FVoxelStateUpdateRequest::Deserialize called, but should not be used."));
		return false;
	}

	
};