#include "Core/UDP/Subscription/FCrowdyDelivery.h"

#include "Utils/SerializationFunctionLibrary.h"

const FCrowdyActorId& FCrowdyDelivery::HeaderActorId() const
{
	return Message->UUID;
}

FGuid FCrowdyDelivery::HeaderGuid() const
{
	return USerializationFunctionLibrary::ToGuid(Message->UUID);
}

int64 FCrowdyDelivery::ChunkX() const
{
	return Message->ChunkX;
}

int64 FCrowdyDelivery::ChunkY() const
{
	return Message->ChunkY;
}

int64 FCrowdyDelivery::ChunkZ() const
{
	return Message->ChunkZ;
}

int64 FCrowdyDelivery::Timestamp() const
{
	return Message->Timestamp;
}

uint8 FCrowdyDelivery::SequenceNumber() const
{
	return Message->SequenceNumber;
}
