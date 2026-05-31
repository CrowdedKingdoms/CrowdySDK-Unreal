#pragma once
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * @brief Represents the response message for a voxel update operation.
 *
 * This class contains information about the result of a voxel update,
 * including the success status and any associated metadata or content changes.
 */
struct FVoxelUpdateResponseMessage : ICrowdyMessage
{
	
	/**
	 * @brief Represents the voxel update response normal vector component along the X-axis.
	 *
	 * This variable stores the X-axis component of the voxel update normal vector
	 * and is utilized as part of the Voxel Update Response Message to convey
	 * direction or orientation information.
	 */
	int16 Vx, Vy, Vz;
	/**
	 * Represents the error code that indicates the result of an operation
	 * or message processing within the Crowdy framework.
	 *
	 * This variable holds a value of type ECrowdyErrorCode, which enumerates
	 * various predefined error codes including success, authentication issues,
	 * invalid requests, and other known error conditions.
	 */
	ECrowdyErrorCode ErrorCode;
	
	/**
	 * @brief Retrieves the type associated with the object or entity.
	 * @return The type of the object or entity as an enumeration or a specific data type.
	 */
	virtual ECrowdyMessageType GetType() const override
	{
		return ECrowdyMessageType::VOXEL_UPDATE_RESPONSE;
	}

	/**
	 * Retrieves the name of the message type.
	 *
	 * This method overrides the base class implementation to provide the
	 * unique name of the FVoxelUpdateResponseMessage type.
	 *
	 * @return An FName instance representing the unique name of the message type, "VoxelUpdateResponseMessage".
	 */
	virtual FName GetTypeName() const override
	{
		return "VoxelUpdateResponseMessage";
	}

	/**
	 * @brief Converts an object or data structure into a format that can be stored or transmitted and reconstructed later.
	 * @return A serialized representation of the object or data structure.
	 */
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

	/**
	 * @brief Deserializes data from a specific format into an object or data structure.
	 * @param Data The serialized data that needs to be deserialized.
	 * @param format The format or schema used for deserialization.
	 */
	virtual bool Deserialize(const TArray<uint8>& Data) override
	{
		int32 Offset = 0;
		if (Data.Num() <= 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("FVoxelUpdateResponseMessage::Deserialize - Data is empty"));
			return false;
		}
		
		SequenceNumber = static_cast<uint8>(Data[Offset]);
		Offset += sizeof(uint8);
		ErrorCode = static_cast<ECrowdyErrorCode>(Data[Offset]);
		return true;
	}

	/**
	 * Calculates and returns the size of the message in bytes based on its internal structure.
	 *
	 * @return The total size of the message in bytes, including the sizes of int64 fields,
	 *         ECrowdyErrorCode, and int16 fields.
	 */
	virtual uint32 GetMessageSize() const override
	{
		return sizeof(int64) * 4 + sizeof(ECrowdyErrorCode) + sizeof(int16) * 3;
	}

	
};
