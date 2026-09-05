#pragma once

#include "Containers/Array.h"
#include "Containers/ArrayView.h"
#include "Containers/UnrealString.h"
#include "CoreMinimal.h"

class ICrowdyMessage;

/**
 * One outbound message reduced to the fields a replication connection needs from it.
 *
 * The frame header, the signature and the tail are not here: the connection writes those itself, and it owns the
 * sequence number, so whatever the message was carrying in its own SequenceNumber field is not carried across.
 *
 * Payload is the payload region alone. For a spatial message that is everything past the fixed metadata header; for
 * a channel message it is the bytes between the length field and the trailing signed marker.
 *
 * Uuid and Payload are views into the serialized frame the split read them out of, not copies, so the frame's bytes
 * have to outlive this object. Copying them here would be pure waste: they were just written by the message's own
 * Serialize, and the connection copies them again when it queues the send.
 */
struct FCrowdyCppOutboundFrame
{
	/** Channel messages use a different frame layout, so they take a different send call. */
	bool bIsChannel = false;

	uint8 Opcode = 0;

	/**
	 * The app id the message wrote into its own header. It is read back for verification only and does not reach the
	 * wire: the connection is configured with one app id for its lifetime and stamps that one on every frame it
	 * sends, because the app id is a facet of the session identity the frame is signed under rather than a per
	 * message value.
	 */
	int64 AppId = 0;

	int64 ChunkX = 0;
	int64 ChunkY = 0;
	int64 ChunkZ = 0;

	/** Channel messages only. */
	int64 ChannelId = 0;

	/** Fan-out instructions the server acts on. Zero for a channel message, which reaches every member. */
	uint8 Distance = 0;
	uint8 Decay = 0;

	/** 32 ASCII octets, not null-terminated. */
	TArrayView<const uint8> Uuid;

	TArrayView<const uint8> Payload;
};

namespace CrowdyCppSend
{
	/**
	 * Take a message apart into the fields a replication connection needs, by serializing it and reading the header
	 * back off its own bytes. Deriving the fields from the serialized frame rather than from the object means the
	 * frame that reaches the wire carries exactly what the message wrote.
	 *
	 * OutFrameStorage receives those serialized bytes and the caller has to keep it alive for as long as it uses the
	 * frame, because the frame's actor id and payload are views into it rather than copies.
	 *
	 * Returns false with a reason for a message that cannot be routed: one that serializes to nothing, one whose
	 * actor id is not 32 octets, one that asked to go unsigned, or one whose declared payload length disagrees with
	 * what it carries. A refusal is not a fallback signal; it means the message was malformed for the wire.
	 */
	CROWDYNET_API bool SplitMessage(const ICrowdyMessage& Message, TArray<uint8>& OutFrameStorage,
		FCrowdyCppOutboundFrame& OutFrame, FString& OutError);
}
