#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "CrowdyCppReplication.h"
#include "Serialization/CrowdyFrame.h"

/**
 * Turns a message the replication connection has already decoded into the frame the message decoders
 * read.
 *
 * There is nothing to parse here: the connection has done that, so this is the point where a routed
 * message joins the path a message received as bytes takes, one step further along. The other
 * transport reaches the same place by reading the envelope off the datagram instead.
 *
 * The frames borrow the connection's receive buffer through the views on the message, so one is valid
 * only for the duration of the callback it was built in.
 *
 * Nothing is re-bounded here, because the two bounds that matter are already established by the time a
 * message reaches this point: the connection builds the actor id from a fixed-size array so it is always
 * exactly 32 octets, and both payload views are sub-ranges of a single received datagram, so neither can
 * exceed what one datagram carries. A check here would be re-asserting the decoder's own postcondition.
 */
namespace CrowdyCppInboundFrame
{
	inline FCrowdyFrame FromSpatial(const FCrowdyCppSpatialMessage& Message)
	{
		FCrowdyFrame Frame;
		Frame.Opcode = Message.Opcode;
		Frame.Body = Message.Payload;
		Frame.Envelope.AppId = Message.AppId;
		Frame.Envelope.ChunkX = Message.ChunkX;
		Frame.Envelope.ChunkY = Message.ChunkY;
		Frame.Envelope.ChunkZ = Message.ChunkZ;
		Frame.Envelope.Uuid = Message.Uuid;
		Frame.Envelope.Timestamp = Message.EpochMillis;
		Frame.Envelope.Sequence = Message.Sequence;
		// The fan-out distance, the decay rate and the signature flag are not carried back off a delivered
		// message. The first two are send-side instructions the server has already acted on, and the third
		// describes a signature the verification that let the frame through has already consumed.
		Frame.bHasEnvelope = true;
		return Frame;
	}

	inline FCrowdyFrame FromChannel(const FCrowdyCppChannelMessage& Message)
	{
		FCrowdyFrame Frame;
		Frame.Opcode = static_cast<uint8>(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION);
		Frame.Body = Message.Payload;
		Frame.Envelope.ChannelId = Message.ChannelId;
		Frame.Envelope.Uuid = Message.SenderUuid;
		Frame.Envelope.Timestamp = Message.EpochMillis;
		Frame.Envelope.Sequence = Message.Sequence;
		Frame.bHasEnvelope = true;
		return Frame;
	}

	/**
	 * A server error frame. It carries no envelope at all, and its two octets sit in the body in the
	 * order the decoder reads them, so the caller owns the storage those two octets live in.
	 */
	inline FCrowdyFrame FromError(const TConstArrayView<uint8> SequenceThenCode)
	{
		FCrowdyFrame Frame;
		Frame.Opcode = static_cast<uint8>(ECrowdyMessageType::GENERIC_ERROR_MESSAGE);
		Frame.Body = SequenceThenCode;
		return Frame;
	}
}
