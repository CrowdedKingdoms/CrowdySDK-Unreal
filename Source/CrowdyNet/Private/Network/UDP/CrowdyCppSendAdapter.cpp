#include "Network/UDP/CrowdyCppSendAdapter.h"

#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Network/UDP/CrowdyCppSendAdapterInternal.h"

THIRD_PARTY_INCLUDES_START
#include "crowdy/wire/protocol.hpp"
THIRD_PARTY_INCLUDES_END

namespace
{
	/**
	 * Offsets of the fields ICrowdyMessage::SerializeMetadata writes, taken from the shared protocol table rather
	 * than counted out here, so this file describes the layout in one place instead of transcribing it a second time.
	 */
	constexpr int32 SpatialOpcodeOffset = static_cast<int32>(crowdy::wire::offsets::kType);
	constexpr int32 SpatialAppIdOffset = static_cast<int32>(crowdy::wire::offsets::kAppId);
	constexpr int32 SpatialChunkXOffset = static_cast<int32>(crowdy::wire::offsets::kChunkX);
	constexpr int32 SpatialChunkYOffset = static_cast<int32>(crowdy::wire::offsets::kChunkY);
	constexpr int32 SpatialChunkZOffset = static_cast<int32>(crowdy::wire::offsets::kChunkZ);
	constexpr int32 SpatialDistanceOffset = static_cast<int32>(crowdy::wire::offsets::kDistance);
	constexpr int32 SpatialDecayOffset = static_cast<int32>(crowdy::wire::offsets::kDecay);
	constexpr int32 SpatialContainsAuthOffset = static_cast<int32>(crowdy::wire::offsets::kContainsAuth);
	constexpr int32 SpatialUuidOffset = static_cast<int32>(crowdy::wire::offsets::kUuid);
	constexpr int32 UuidSize = static_cast<int32>(crowdy::wire::kUuidSize);
	constexpr int32 SpatialHeaderSize = static_cast<int32>(crowdy::wire::kLongSpatialHeaderSize);

	// Each field is pinned to the end of the one before it, so a field added to, removed from, resized in or moved
	// within the shared layout table fails the build here rather than shifting where every later field is read from.
	// Pinning only the header's total size would miss a change that leaves the total intact.
	static_assert(SpatialOpcodeOffset == 0,
		"The opcode is no longer the first octet of a spatial frame.");
	static_assert(SpatialAppIdOffset == SpatialOpcodeOffset + static_cast<int32>(sizeof(uint8)),
		"The app id no longer follows the opcode.");
	static_assert(SpatialChunkXOffset == SpatialAppIdOffset + static_cast<int32>(sizeof(int64)),
		"The chunk x coordinate no longer follows the app id.");
	static_assert(SpatialChunkYOffset == SpatialChunkXOffset + static_cast<int32>(sizeof(int64)),
		"The chunk y coordinate no longer follows the chunk x coordinate.");
	static_assert(SpatialChunkZOffset == SpatialChunkYOffset + static_cast<int32>(sizeof(int64)),
		"The chunk z coordinate no longer follows the chunk y coordinate.");
	static_assert(SpatialDistanceOffset == SpatialChunkZOffset + static_cast<int32>(sizeof(int64)),
		"The replication distance no longer follows the chunk z coordinate.");
	static_assert(SpatialDecayOffset == SpatialDistanceOffset + static_cast<int32>(sizeof(uint8)),
		"The decay rate no longer follows the replication distance.");
	static_assert(SpatialContainsAuthOffset == SpatialDecayOffset + static_cast<int32>(sizeof(uint8)),
		"The signed marker no longer follows the decay rate.");
	static_assert(SpatialUuidOffset == SpatialContainsAuthOffset + static_cast<int32>(sizeof(uint8)),
		"The actor id no longer follows the signed marker.");
	static_assert(SpatialHeaderSize == SpatialUuidOffset + UuidSize,
		"The payload no longer starts where the actor id ends.");
	// The comparisons below are the ones with a side that comes from the header writer's own description of the
	// layout rather than from the table, which makes them the ones that catch the two drifting apart. They pin
	// where each field goes; which value is written there is covered by the byte parity coverage.
	static_assert(SpatialHeaderSize == MetadataSize + 1,
		"The spatial header is no longer the size the message metadata adds up to.");
	static_assert(SpatialOpcodeOffset == CrowdySpatialHeader::OpcodeOffset,
		"The header writer no longer puts the opcode where the layout table does.");
	static_assert(SpatialAppIdOffset == CrowdySpatialHeader::AppIdOffset,
		"The header writer no longer puts the app id where the layout table does.");
	static_assert(SpatialChunkXOffset == CrowdySpatialHeader::ChunkXOffset,
		"The header writer no longer puts the chunk x coordinate where the layout table does.");
	static_assert(SpatialChunkYOffset == CrowdySpatialHeader::ChunkYOffset,
		"The header writer no longer puts the chunk y coordinate where the layout table does.");
	static_assert(SpatialChunkZOffset == CrowdySpatialHeader::ChunkZOffset,
		"The header writer no longer puts the chunk z coordinate where the layout table does.");
	static_assert(SpatialDistanceOffset == CrowdySpatialHeader::DistanceOffset,
		"The header writer no longer puts the replication distance where the layout table does.");
	static_assert(SpatialDecayOffset == CrowdySpatialHeader::DecayOffset,
		"The header writer no longer puts the decay rate where the layout table does.");
	static_assert(SpatialContainsAuthOffset == CrowdySpatialHeader::ContainsAuthOffset,
		"The header writer no longer puts the signed marker where the layout table does.");
	static_assert(SpatialUuidOffset == CrowdySpatialHeader::ActorIdOffset,
		"The header writer no longer puts the actor id where the layout table does.");
	static_assert(UuidSize == FCrowdyActorId::NumOctets,
		"An actor id is no longer the number of octets the layout table reserves for it.");

	/** Offsets of the fields FChannelMessageRequest::Serialize writes. The signed marker trails the payload. */
	constexpr int32 ChannelOpcodeOffset = 0;
	constexpr int32 ChannelIdOffset = static_cast<int32>(crowdy::wire::channel::kChannelIdOffset);
	constexpr int32 ChannelUuidOffset = static_cast<int32>(crowdy::wire::channel::kUuidOffset);
	constexpr int32 ChannelPayloadLengthOffset = static_cast<int32>(crowdy::wire::channel::kPayloadLenOffset);
	constexpr int32 ChannelHeaderSize = static_cast<int32>(crowdy::wire::channel::kHeaderSize);
	constexpr int32 ChannelTrailerSize = static_cast<int32>(sizeof(uint8));

	static_assert(ChannelIdOffset == ChannelOpcodeOffset + static_cast<int32>(sizeof(uint8)),
		"The channel id no longer follows the opcode.");
	static_assert(ChannelUuidOffset == ChannelIdOffset + static_cast<int32>(sizeof(int64)),
		"The sender id no longer follows the channel id.");
	static_assert(ChannelPayloadLengthOffset == ChannelUuidOffset + UuidSize,
		"The payload length no longer follows the sender id.");
	static_assert(ChannelHeaderSize == ChannelPayloadLengthOffset + static_cast<int32>(sizeof(uint16)),
		"The payload no longer starts where its length field ends.");

	// What we hand the connection ends at the signed marker: the signature, the token and the sequence number are the
	// connection's to append. Stating the request tail as the sum of its parts catches one of them being added or
	// dropped. It cannot catch one of them changing size, because the same term then moves on both sides.
	static_assert(static_cast<int32>(crowdy::wire::channel::kRequestTailSize) == ChannelTrailerSize
			+ static_cast<int32>(crowdy::wire::kHmacTagSize) + static_cast<int32>(sizeof(int64))
			+ static_cast<int32>(sizeof(uint8)),
		"The channel request tail is no longer the signed marker plus the three fields the connection appends.");

	int64 ReadInt64(const TArrayView<const uint8> Frame, const int32 Offset)
	{
		int64 Value = 0;
		FMemory::Memcpy(&Value, Frame.GetData() + Offset, sizeof(int64));
		return Value;
	}

	uint16 ReadUInt16(const TArrayView<const uint8> Frame, const int32 Offset)
	{
		uint16 Value = 0;
		FMemory::Memcpy(&Value, Frame.GetData() + Offset, sizeof(uint16));
		return Value;
	}

	bool SplitChannelFrame(const TArrayView<const uint8> Frame, FCrowdyCppOutboundFrame& OutFrame, FString& OutError)
	{
		if (Frame.Num() < ChannelHeaderSize + ChannelTrailerSize)
		{
			OutError = FString::Printf(TEXT("a channel frame is at least %d octets, and this one is %d"),
				ChannelHeaderSize + ChannelTrailerSize, Frame.Num());
			return false;
		}

		// The connection signs every frame it sends. A message that serialized itself as unsigned would go out
		// signed, which is a different frame from the one the caller asked for, so refuse rather than reshape it.
		if (Frame[Frame.Num() - 1] != 1)
		{
			OutError = TEXT("the channel frame is not marked as signed");
			return false;
		}

		const int32 CarriedPayload = Frame.Num() - ChannelHeaderSize - ChannelTrailerSize;
		const int32 DeclaredPayload = static_cast<int32>(ReadUInt16(Frame, ChannelPayloadLengthOffset));

		// The length field and the payload are written from the same array, so a disagreement means the frame was
		// built by something other than the encoder these offsets describe.
		if (CarriedPayload != DeclaredPayload)
		{
			OutError = FString::Printf(TEXT("the channel frame declares %d payload octets and carries %d"),
				DeclaredPayload, CarriedPayload);
			return false;
		}

		OutFrame.bIsChannel = true;
		OutFrame.Opcode = Frame[ChannelOpcodeOffset];
		OutFrame.ChannelId = ReadInt64(Frame, ChannelIdOffset);
		OutFrame.Uuid = Frame.Slice(ChannelUuidOffset, UuidSize);
		OutFrame.Payload = Frame.Slice(ChannelHeaderSize, CarriedPayload);
		return true;
	}

	bool SplitSpatialFrame(const TArrayView<const uint8> Frame, FCrowdyCppOutboundFrame& OutFrame, FString& OutError)
	{
		if (Frame.Num() < SpatialHeaderSize)
		{
			OutError = FString::Printf(TEXT("a spatial frame is at least %d octets, and this one is %d"),
				SpatialHeaderSize, Frame.Num());
			return false;
		}

		if (Frame[SpatialContainsAuthOffset] != 1)
		{
			OutError = TEXT("the spatial frame is not marked as signed");
			return false;
		}

		OutFrame.Opcode = Frame[SpatialOpcodeOffset];
		OutFrame.AppId = ReadInt64(Frame, SpatialAppIdOffset);
		OutFrame.ChunkX = ReadInt64(Frame, SpatialChunkXOffset);
		OutFrame.ChunkY = ReadInt64(Frame, SpatialChunkYOffset);
		OutFrame.ChunkZ = ReadInt64(Frame, SpatialChunkZOffset);
		OutFrame.Distance = Frame[SpatialDistanceOffset];
		OutFrame.Decay = Frame[SpatialDecayOffset];
		OutFrame.Uuid = Frame.Slice(SpatialUuidOffset, UuidSize);
		OutFrame.Payload = Frame.Slice(SpatialHeaderSize, Frame.Num() - SpatialHeaderSize);
		return true;
	}
}

bool CrowdyCppSend::SplitSerializedFrame(const TArrayView<const uint8> Frame, FCrowdyCppOutboundFrame& OutFrame,
	FString& OutError)
{
	OutFrame = FCrowdyCppOutboundFrame();
	OutError.Reset();

	if (Frame.Num() < 1)
	{
		OutError = TEXT("the frame is empty");
		return false;
	}

	if (static_cast<ECrowdyMessageType>(Frame[0]) == ECrowdyMessageType::CHANNEL_MESSAGE_REQUEST)
	{
		return SplitChannelFrame(Frame, OutFrame, OutError);
	}

	return SplitSpatialFrame(Frame, OutFrame, OutError);
}

bool CrowdyCppSend::SplitMessage(const ICrowdyMessage& Message, TArray<uint8>& OutFrameStorage,
	FCrowdyCppOutboundFrame& OutFrame, FString& OutError)
{
	// Cleared before anything can fail, so a refused split cannot leave the caller holding views into bytes that
	// describe some earlier message.
	OutFrame = FCrowdyCppOutboundFrame();
	OutFrameStorage.Reset();

	// The actor id is written into the header with no padding and no length. A message that never received one
	// addresses nobody, so it is refused here rather than sent with the field blank.
	if (!Message.UUID.IsSet())
	{
		OutError = FString::Printf(
			TEXT("the message has no actor id, so it cannot fill the %d octets the header reserves for one"), UuidSize);
		return false;
	}

	OutFrameStorage = Message.Serialize();
	if (OutFrameStorage.Num() < 1)
	{
		OutError = TEXT("the message serialized to nothing");
		return false;
	}

	return SplitSerializedFrame(OutFrameStorage, OutFrame, OutError);
}
