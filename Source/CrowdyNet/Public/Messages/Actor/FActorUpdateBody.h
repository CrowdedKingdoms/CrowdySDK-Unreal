#pragma once

#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Utils/SerializationFunctionLibrary.h"

/**
 * The payload an actor update carries, shared by the outbound request and the inbound notification so
 * that one description of the layout serves both directions.
 *
 * The layout is a four-octet declared length followed by that many state octets.
 */
struct FActorUpdateBody : ICrowdyMessage
{
	/**
	 * How many state octets the message declares. It is written out as the sender set it and is never
	 * derived from StateBytes, so a sender that sets one without the other is describing a frame no
	 * reader can use rather than quietly getting a corrected one.
	 */
	int32 StateSize = 0;

protected:

	/**
	 * The state octets a REQUEST owns and sends. Re-exposed by FActorUpdateRequestMessage and hidden from
	 * the inbound notification, which does not own its bytes: it points at the receive buffer through its
	 * own StateView instead, and an empty array here would otherwise read as an update that carried nothing.
	 */
	TArray<uint8> StateBytes;

	/**
	 * Exactly what AppendBody will write, so the frame can be sized once before anything is appended.
	 * Measured from StateBytes rather than from the declared StateSize, because it describes what is
	 * actually written and a sender is free to declare a length it did not supply.
	 */
	int32 BodySize() const
	{
		return sizeof(StateSize) + StateBytes.Num();
	}

	void AppendBody(TArray<uint8>& Data) const
	{
		USerializationFunctionLibrary::AppendValue(Data, StateSize);
		Data.Append(StateBytes);
	}
};
