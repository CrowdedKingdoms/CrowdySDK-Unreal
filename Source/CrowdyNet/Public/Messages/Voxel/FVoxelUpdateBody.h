#pragma once

#include "CrowdyNetLog.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * The payload a voxel update carries, shared by the outbound request and the inbound notification so
 * that one description of the layout serves both directions.
 *
 * The layout is the voxel's position within its chunk, its type, a declared state length, and then the
 * state octets when the message carries any.
 */
struct FVoxelUpdateBody : ICrowdyMessage
{
	/** The voxel's coordinates within the chunk the message names. */
	int16 Vx = 0;
	int16 Vy = 0;
	int16 Vz = 0;

	/** What kind of voxel this is, in whatever numbering the voxel system uses. */
	int16 VoxelType = 0;

	TArray<uint8> StateBytes;

	/**
	 * How many state octets the message declares. It is written out as the sender set it, so a message
	 * can advertise a length while carrying no state at all. That combination is legal on this layout
	 * and readers treat it as truncated, which is why the length is never derived from StateBytes here.
	 */
	uint16 StateSize = 0;

	/** Whether the state octets are actually sent. The declared length above is written either way. */
	bool bContainsState = false;

protected:

	void AppendBody(TArray<uint8>& Data) const
	{
		USerializationFunctionLibrary::AppendValue(Data, Vx);
		USerializationFunctionLibrary::AppendValue(Data, Vy);
		USerializationFunctionLibrary::AppendValue(Data, Vz);
		USerializationFunctionLibrary::AppendValue(Data, VoxelType);
		USerializationFunctionLibrary::AppendValue(Data, StateSize);

		if (bContainsState && StateBytes.Num() > 0)
		{
			Data.Append(StateBytes);
			UE_CLOG(CrowdyNetTrace::Serialize(), LogCrowdyNet, Log, TEXT("State Appended in message. State Size %d"), StateBytes.Num());
		}
	}
};
