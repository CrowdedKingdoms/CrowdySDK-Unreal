#pragma once
#include "CrowdyNetLog.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Messages/Voxel/FVoxelUpdateBody.h"
#include "Serialization/CrowdyFrame.h"
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
struct FVoxelUpdateNotificationMessage : FVoxelUpdateBody
{
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
	 * Deserializes the provided frame into the member variables of this class.
	 * The method extracts and assigns values for MapID, chunk coordinates, voxel coordinates,
	 * voxel type, and optionally the voxel state if the data length permits.
	 *
	 * @param Frame The frame carrying the voxel update data to deserialize.
	 */
	[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
	{
		const TConstArrayView<uint8> Data = Frame.Body;
		const int32 DataLength = Data.Num();

		if (DataLength <= 0)
			return false;

		int32 Offset = 0;

		ApplyEnvelope(Frame);
		if (!ApplyEnvelopeActorId(Frame))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FVoxelUpdateNotificationMessage::DecodePayload - the frame carries no actor id"));
			return false;
		}

		if (!USerializationFunctionLibrary::DeserializeValue(Data, Vx, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FVoxelUpdateNotificationMessage::DecodePayload - Vx deserialization failed"));
			 return false;
		}
		Offset += sizeof(int16);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, Vy, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FVoxelUpdateNotificationMessage::DecodePayload - Vy deserialization failed"));
			 return false;
		}
		Offset += sizeof(int16);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, Vz, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FVoxelUpdateNotificationMessage::DecodePayload - Vz deserialization failed"));
			return false;
		}
		Offset += sizeof(int16);
		
		if (!USerializationFunctionLibrary::DeserializeValue(Data, VoxelType, Offset))
		{
			UE_LOG(LogCrowdyNet, Warning, TEXT("FVoxelUpdateNotificationMessage::DecodePayload - VoxelType deserialization failed"));
			return false;
		}
		
		Offset += sizeof(int16);
		
		if (Offset + sizeof(uint16) <= DataLength)
		{
			if (!USerializationFunctionLibrary::DeserializeValue(Data, StateSize, Offset))
			{
				UE_LOG(LogCrowdyNet, Warning, TEXT("FVoxelUpdateNotificationMessage::DecodePayload - StateSize deserialization failed"));
				return false;
			}
			
			Offset += sizeof(uint16);
			
			// The declared length is attacker-controlled, so it has to be checked against the bytes that
			// are actually present before it is used to size a copy. Comparing against the remainder
			// rather than adding to the offset keeps the check itself free of overflow.
			if (StateSize > 0 && StateSize > static_cast<uint32>(DataLength - Offset))
			{
				UE_LOG(LogCrowdyNet, Warning,
					TEXT("FVoxelUpdateNotificationMessage::DecodePayload - declared state length %u runs past the end of a %d byte frame"),
					StateSize, DataLength);
				return false;
			}

			if (StateSize > 0 && StateSize < 5000)
			{
				bContainsState = true;
				StateBytes.SetNumUninitialized(StateSize);
				FMemory::Memcpy(StateBytes.GetData(), Data.GetData() + Offset, StateSize);
			}
			else if (StateSize > 0)
			{
				// The state fits the frame but is larger than this decoder will accept, so the update is
				// reported with no state at all. Say so: a caller cannot otherwise tell this apart from an
				// update that legitimately carried none, and treating one as the other overwrites live
				// state with defaults.
				UE_LOG(LogCrowdyNet, Warning,
					TEXT("FVoxelUpdateNotificationMessage::DecodePayload - state of %u bytes exceeds the accepted maximum, reporting the update without it"),
					StateSize);
			}
			
			return true;
		}
		
		UE_LOG(LogCrowdyNet, Warning, TEXT("FVoxelUpdateNotificationMessage::DecodePayload - StateSize deserialization failed"));
		return false;

	}

	/** Receive-only: a voxel update is sent as FVoxelStateUpdateRequest, not as this. */
	virtual TArray<uint8> Serialize() const override
	{
		return TArray<uint8>();
	}

};

