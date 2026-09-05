#pragma once

#include "CoreMinimal.h"
#include "Serialization/CrowdyFrame.h"

/**
 * Reads the envelope off a received frame, leaving the frame's Body holding only the bytes that
 * belong to its opcode.
 *
 * There is one of these per envelope shape rather than one per opcode, because the protocol has two:
 * the long spatial header and the shorter channel header. Which one an opcode uses is decided where
 * the opcode is dispatched.
 *
 * The split is what lets a message be decoded the same way however it arrived. A transport that
 * receives datagrams splits the bytes here; a transport that is handed a frame already decoded fills
 * the envelope in directly and never calls these.
 */
namespace CrowdyFrameSplit
{
	/**
	 * The long spatial header, then the signature and trailer off the end. Returns false when the
	 * bytes are too short to hold the header, or too short to hold the trailer the header's own
	 * signature flag says is there, which is a frame no decoder for a spatial opcode can read.
	 */
	[[nodiscard]] CROWDYNET_API bool ReadSpatialEnvelope(FCrowdyFrame& Frame);

	/**
	 * The channel header, then the payload the header declares. Returns false when the bytes are too
	 * short to hold the header, or when the declared payload length runs past the end.
	 *
	 * The trailer is optional here, matching the wire: a notification that carries no timestamp and
	 * sequence still splits, and those envelope fields keep their defaults.
	 */
	[[nodiscard]] CROWDYNET_API bool ReadChannelEnvelope(FCrowdyFrame& Frame);

	/**
	 * Whichever of the two the frame's opcode carries, or neither, in which case the frame is left with
	 * its whole body and this reports success.
	 *
	 * This is where an opcode is mapped to its envelope shape, so a transport that receives bytes calls
	 * only this and never has to know which opcodes carry what.
	 */
	[[nodiscard]] CROWDYNET_API bool ReadEnvelopeForOpcode(FCrowdyFrame& Frame);
}
