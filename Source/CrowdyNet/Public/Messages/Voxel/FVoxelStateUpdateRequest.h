#pragma once

#include "CrowdyNetLog.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Messages/Voxel/FVoxelUpdateBody.h"
#include "Serialization/CrowdyFrame.h"
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
struct FVoxelStateUpdateRequest : FVoxelUpdateBody
{
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

	/** The spatial header followed by the voxel body. */
	virtual TArray<uint8> Serialize() const override
	{
		TArray<uint8> Data = SerializeMetadata();
		AppendBody(Data);
		return Data;
	}

	/** Send-only: a voxel update arrives as FVoxelUpdateNotificationMessage, not as this. */
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("FVoxelStateUpdateRequest::DecodePayload called, but should not be used."));
		return false;
	}

	
};