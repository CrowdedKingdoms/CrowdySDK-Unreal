#include "Serialization/CrowdyFrameSplit.h"

#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Utils/SerializationFunctionLibrary.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/wire/protocol.hpp"
THIRD_PARTY_INCLUDES_END

// Named for this file rather than for the layout, because the send adapter describes the same fields
// with the same obvious names at datagram offsets, one octet further along. Two file-local constants of
// one name and two values in one module is the shape that breaks a unity build and misleads a reader
// long before that.
namespace CrowdyFrameSplitLayout
{
	// Offsets within a frame's bytes, which start one octet later than the datagram the protocol
	// constants describe, since the opcode has already been taken off.
	constexpr int32 SpatialAppId = static_cast<int32>(crowdy::wire::offsets::kAppId) - 1;
	constexpr int32 SpatialChunkX = static_cast<int32>(crowdy::wire::offsets::kChunkX) - 1;
	constexpr int32 SpatialChunkY = static_cast<int32>(crowdy::wire::offsets::kChunkY) - 1;
	constexpr int32 SpatialChunkZ = static_cast<int32>(crowdy::wire::offsets::kChunkZ) - 1;
	constexpr int32 SpatialDistance = static_cast<int32>(crowdy::wire::offsets::kDistance) - 1;
	constexpr int32 SpatialDecay = static_cast<int32>(crowdy::wire::offsets::kDecay) - 1;
	constexpr int32 SpatialAuth = static_cast<int32>(crowdy::wire::offsets::kContainsAuth) - 1;
	constexpr int32 SpatialUuid = static_cast<int32>(crowdy::wire::offsets::kUuid) - 1;
	constexpr int32 SpatialBody = static_cast<int32>(crowdy::wire::offsets::kPayload) - 1;

	constexpr int32 UuidOctets = static_cast<int32>(crowdy::wire::kUuidSize);
	constexpr int32 SignatureOctets = static_cast<int32>(crowdy::wire::kHmacTagSize);
	constexpr int32 TrailerOctets = static_cast<int32>(crowdy::wire::kTailNoHmac);

	constexpr int32 ChannelId = static_cast<int32>(crowdy::wire::channel::kChannelIdOffset) - 1;
	constexpr int32 ChannelUuid = static_cast<int32>(crowdy::wire::channel::kUuidOffset) - 1;
	constexpr int32 ChannelPayloadLen = static_cast<int32>(crowdy::wire::channel::kPayloadLenOffset) - 1;
	constexpr int32 ChannelBody = static_cast<int32>(crowdy::wire::channel::kPayloadOffset) - 1;

	// What this chain does and does not buy. It pins the vendored table's internal consistency, which is
	// what a re-vendor breaks, and it pins the one place a second description still exists: MetadataSize,
	// which the send path writes headers from. It constrains no decoder, because after the hoist no
	// message struct reads any of these offsets; the byte parity suite is what covers that.
	static_assert(SpatialBody == MetadataSize,
		"The spatial header is no longer the size the send path's metadata block adds up to.");

	// The field order is what makes the offsets above readable as a layout rather than as a list of
	// numbers, so a re-vendor that reordered them would otherwise change meaning silently.
	static_assert(SpatialAppId == 0 && SpatialChunkX == 8 && SpatialChunkY == 16 && SpatialChunkZ == 24
			&& SpatialDistance == 32 && SpatialDecay == 33 && SpatialAuth == 34 && SpatialUuid == 35,
		"The spatial header fields are no longer in the order and at the sizes this reader reads them.");
	static_assert(SpatialUuid + UuidOctets == SpatialBody,
		"The spatial actor id no longer runs up to the start of the body.");
	static_assert(static_cast<int32>(crowdy::wire::channel::kNotificationTailSize) == TrailerOctets,
		"A channel notification no longer ends with the same trailer as a spatial frame.");
	static_assert(ChannelId == 0 && ChannelUuid == 8 && ChannelPayloadLen == 40 && ChannelBody == 42,
		"The channel header fields are no longer in the order and at the sizes this reader reads them.");
}

bool CrowdyFrameSplit::ReadSpatialEnvelope(FCrowdyFrame& Frame)
{
	// Scoped to the function rather than the file: these names are ordinary enough that a file compiled
	// alongside this one could declare its own, and a file-wide using-declaration would make the two
	// collide.
	using namespace CrowdyFrameSplitLayout;

	const TConstArrayView<uint8> Data = Frame.Payload;
	if (Data.Num() < SpatialBody)
	{
		return false;
	}

	// The signature flag decides how much of the end is trailer, so it is read before anything is
	// measured against the end of the frame.
	const bool bContainsAuth = Data[SpatialAuth] != 0;
	const int32 EndBytes = (bContainsAuth ? SignatureOctets : 0) + TrailerOctets;

	// Written as a comparison against the bytes the header leaves behind rather than as a sum
	// against the length, so a frame shorter than its own header cannot wrap into a passing check.
	if (Data.Num() - SpatialBody < EndBytes)
	{
		return false;
	}

	FCrowdyEnvelope& Envelope = Frame.Envelope;
	int32 Offset = SpatialAppId;

	if (!USerializationFunctionLibrary::DeserializeValue(Data, Envelope.AppId, Offset))
	{
		return false;
	}

	Offset = SpatialChunkX;
	if (!USerializationFunctionLibrary::DeserializeValue(Data, Envelope.ChunkX, Offset))
	{
		return false;
	}

	Offset = SpatialChunkY;
	if (!USerializationFunctionLibrary::DeserializeValue(Data, Envelope.ChunkY, Offset))
	{
		return false;
	}

	Offset = SpatialChunkZ;
	if (!USerializationFunctionLibrary::DeserializeValue(Data, Envelope.ChunkZ, Offset))
	{
		return false;
	}

	Envelope.Distance = Data[SpatialDistance];
	Envelope.Decay = Data[SpatialDecay];
	Envelope.bContainsAuth = bContainsAuth;
	Envelope.Uuid = TConstArrayView<uint8>(Data.GetData() + SpatialUuid, UuidOctets);

	// The trailer sits at the very end whatever the body's length, and the signature, when there is
	// one, sits between the two.
	int32 TrailerOffset = Data.Num() - TrailerOctets;
	if (!USerializationFunctionLibrary::DeserializeValue(Data, Envelope.Timestamp, TrailerOffset))
	{
		return false;
	}
	Envelope.Sequence = Data[Data.Num() - 1];

	Frame.Body = TConstArrayView<uint8>(Data.GetData() + SpatialBody,
		Data.Num() - SpatialBody - EndBytes);
	Frame.bHasEnvelope = true;
	return true;
}

bool CrowdyFrameSplit::ReadEnvelopeForOpcode(FCrowdyFrame& Frame)
{
	switch (static_cast<ECrowdyMessageType>(Frame.Opcode))
	{
	case ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION:
	case ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION:
	case ECrowdyMessageType::SERVER_EVENT_NOTIFICATION:
	case ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION:
	case ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION:
	case ECrowdyMessageType::CLIENT_TEXT_NOTIFICATION:
	case ECrowdyMessageType::GENERIC_SPATIAL_1:
	case ECrowdyMessageType::SINGLE_ACTOR_MESSAGE:
		return ReadSpatialEnvelope(Frame);

	case ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION:
		return ReadChannelEnvelope(Frame);

	default:
		return true;
	}
}

bool CrowdyFrameSplit::ReadChannelEnvelope(FCrowdyFrame& Frame)
{
	using namespace CrowdyFrameSplitLayout;

	const TConstArrayView<uint8> Data = Frame.Payload;
	if (Data.Num() < ChannelBody)
	{
		return false;
	}

	FCrowdyEnvelope& Envelope = Frame.Envelope;
	int32 Offset = ChannelId;

	if (!USerializationFunctionLibrary::DeserializeValue(Data, Envelope.ChannelId, Offset))
	{
		return false;
	}

	Envelope.Uuid = TConstArrayView<uint8>(Data.GetData() + ChannelUuid, UuidOctets);

	uint16 BodyLength = 0;
	Offset = ChannelPayloadLen;
	if (!USerializationFunctionLibrary::DeserializeValue(Data, BodyLength, Offset))
	{
		return false;
	}

	if (Data.Num() - ChannelBody < BodyLength)
	{
		return false;
	}

	// The trailer is part of the layout rather than an optional extra: a notification always carries the
	// server's timestamp and sequence behind its payload. Refusing one without them is what keeps this
	// reader and the vendored decoder agreeing about which frames are messages, since the vendored one
	// refuses the same frame and a message only one of the two decoders accepts is worse than a message
	// neither does.
	int32 TrailerOffset = ChannelBody + BodyLength;
	if (Data.Num() - TrailerOffset < TrailerOctets)
	{
		return false;
	}

	if (!USerializationFunctionLibrary::DeserializeValue(Data, Envelope.Timestamp, TrailerOffset))
	{
		return false;
	}
	Envelope.Sequence = Data[TrailerOffset + TrailerOctets - 1];

	Frame.Body = TConstArrayView<uint8>(Data.GetData() + ChannelBody, BodyLength);
	Frame.bHasEnvelope = true;
	return true;
}
