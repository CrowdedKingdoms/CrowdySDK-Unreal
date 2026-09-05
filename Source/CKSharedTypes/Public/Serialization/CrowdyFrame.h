#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

/**
 * The parts of a received message that surround its body.
 *
 * A spatial message and a channel message carry different envelopes, and a few messages carry none
 * at all, so a field that does not apply to the frame it sits on keeps its default. Which fields are
 * meaningful is decided by the opcode, and each is noted below.
 *
 * The values are read off the wire and are no more trustworthy than the rest of the message.
 * Timestamp in particular is supplied by the server with no range guarantee.
 */
struct FCrowdyEnvelope
{
	/** Spatial frames only. */
	int64 AppId = 0;
	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;

	/** Channel frames only. */
	int64 ChannelId = 0;

	/** Server time in milliseconds since the epoch. */
	int64 Timestamp = 0;

	/**
	 * The 32 octets that address the sending actor. Borrowed from the frame's bytes and subject to
	 * the same lifetime rule.
	 *
	 * Almost always 32 ASCII hex digits, but not always: a server-originated event writes two binary
	 * GUIDs here instead, and a server-originated channel notification has no sender and leaves the
	 * region empty or filled with something that is not text. So it is carried as octets and read as
	 * text only by the messages whose layout says it is text.
	 */
	TConstArrayView<uint8> Uuid;

	/** Spatial frames only. The server's fan-out instructions, which it has already acted on. */
	uint8 Distance = 0;
	uint8 Decay = 0;

	/** The sending client's sequence number, or the server's for a message the server originated. */
	uint8 Sequence = 0;

	/**
	 * Whether the frame carried a server signature. A frame that carried one had it checked before
	 * it got here; a frame that did not is unverified, which the protocol permits.
	 */
	bool bContainsAuth = false;
};

/**
 * One received message, already separated from the datagram that carried it and from its envelope.
 *
 * A datagram carries either a single message or several packed together, and only the transport that
 * received it knows which. It performs that split once and passes each message on as a frame, so a
 * decoder reads a message without knowing how it arrived.
 *
 * The bytes are borrowed, not owned: they point into the receive buffer the transport is still
 * holding, and that buffer is reused or released once the transport moves on.
 *
 * A decoder may keep a view into Body rather than copying the octets out, and several do, so the
 * obligation runs the other way from what a frame's own scope suggests: whoever supplies the frame
 * must keep its bytes valid until the message built from it has finished being delivered, not merely
 * until the decode call returns. Decode and dispatch therefore belong in the same scope as the bytes.
 */
struct FCrowdyFrame
{
	/**
	 * The whole message except the leading opcode, envelope and trailer included.
	 *
	 * Only the code that splits a frame reads this. A decoder reads Body, which is the part of the
	 * message that belongs to its own opcode.
	 */
	TConstArrayView<uint8> Payload;

	/**
	 * The opcode-specific bytes: whatever envelope the opcode carries has been taken off the front,
	 * and the signature and trailer off the end. Offset 0 is the first byte a decoder for this opcode
	 * owns, and the last byte is the last byte it owns, so a layout that runs to the end of its own
	 * region still finds it at the end of this view.
	 *
	 * For an opcode that carries no envelope this is the whole message after the opcode.
	 */
	TConstArrayView<uint8> Body;

	FCrowdyEnvelope Envelope;

	/** The opcode byte that led the message. It is not part of Payload or Body. */
	uint8 Opcode = 0;

	/** True when this message arrived packed alongside others rather than alone in its datagram. */
	bool bBundleMember = false;

	/** True once an envelope has been read off this frame. False for an opcode that carries none. */
	bool bHasEnvelope = false;

	FCrowdyFrame() = default;

	FCrowdyFrame(const uint8 InOpcode, const TConstArrayView<uint8> InPayload,
		const bool bInBundleMember = false)
		: Payload(InPayload)
		, Body(InPayload)
		, Opcode(InOpcode)
		, bBundleMember(bInBundleMember)
	{
	}

	/**
	 * Splits one whole message, opcode byte included, into its opcode and its bytes. Returns false for
	 * an empty run of bytes, which carries no opcode and therefore describes no message.
	 *
	 * The frame it produces has no envelope read off it yet, so Body is everything after the opcode.
	 */
	[[nodiscard]] static bool FromMessageBytes(const TConstArrayView<uint8> MessageBytes, FCrowdyFrame& OutFrame,
		const bool bInBundleMember = false)
	{
		if (MessageBytes.IsEmpty())
		{
			return false;
		}

		OutFrame.Opcode = MessageBytes[0];
		OutFrame.Payload = TConstArrayView<uint8>(MessageBytes.GetData() + 1, MessageBytes.Num() - 1);
		OutFrame.Body = OutFrame.Payload;
		OutFrame.Envelope = FCrowdyEnvelope();
		OutFrame.bBundleMember = bInBundleMember;
		OutFrame.bHasEnvelope = false;
		return true;
	}
};
