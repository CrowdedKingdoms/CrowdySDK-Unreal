#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"

namespace CrowdyAudioPacket
{
	/** The most octets one audio payload carries: the datagram less the spatial header and the signed tail. */
	constexpr int32 MaxPayloadBytes = 1123;
}

/**
 * The payload a voice packet carries, shared by the outbound request and the inbound notification.
 *
 * The network layer treats it as opaque: whatever the sender puts after the message header is exactly what
 * every receiver gets back. What it means (codec, sequence, timing) is for the voice layer to read.
 */
struct FClientAudioBody : ICrowdyMessage
{
	TArray<uint8> Payload;
};
