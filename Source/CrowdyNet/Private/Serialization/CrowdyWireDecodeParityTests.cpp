#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CrowdyWireParitySupport.h"
#include "Serialization/CrowdyFrame.h"
#include "Messages/FPingTestMessage.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Messages/Communication/FClientAudioNotification.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Messages/GameObjects/FServerEventNotification.h"
#include "Messages/Voxel/FVoxelUpdateNotificationMessage.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyWireDecodeParityTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// FCrowdyMessageParser removes the leading opcode byte before handing a datagram to a message, so a
	// message's DecodePayload is not the inverse of its Serialize: every offset inside it is shifted down by
	// one. Decoding a datagram straight from an encoder therefore has to strip that byte first.
	const int32 WireDecodeParityOpcodeByte = 1;

	// The metadata block a message decodes after the opcode byte, and the unsigned trailer behind it.
	const int32 WireDecodeParityMetadataEnd = MetadataSize;
	const int32 WireDecodeParityTail = TailSize;

	// Offsets inside a stripped datagram, i.e. relative to the first metadata byte. Each one is built
	// from the layout constants both implementations share rather than from a copied number, so a layout
	// change moves the harness with it instead of leaving it asserting against a stale offset.
	const int32 WireDecodeParityVoxelStateLenOffset =
		MetadataSize + static_cast<int32>(crowdy::wire::voxel::kStateLenOffset);
	const int32 WireDecodeParityVoxelStateOffset =
		MetadataSize + static_cast<int32>(crowdy::wire::voxel::kFixedSize);
	const int32 WireDecodeParityEventStateOffset =
		MetadataSize + static_cast<int32>(crowdy::wire::kEventTypeSize) + static_cast<int32>(sizeof(int32));
	const int32 WireDecodeParityServerEventStateOffset =
		MetadataSize + static_cast<int32>(crowdy::wire::kEventTypeSize);
	const int32 WireDecodeParityActorStateOffset = MetadataSize + static_cast<int32>(sizeof(int32));
	const int32 WireDecodeParityChannelPayloadOffset =
		static_cast<int32>(sizeof(int64)) + static_cast<int32>(crowdy::wire::kUuidSize)
		+ static_cast<int32>(sizeof(uint16));

	// The identity region of a server-originated event frame: everything before it is the four chunk
	// coordinates and the three flag bytes.
	const int32 WireDecodeParityIdentityOffset =
		static_cast<int32>(sizeof(int64)) * 4 + 3 * static_cast<int32>(sizeof(uint8));

	// Base for the tests that deliberately feed malformed bytes to a decoder. Those decoders report
	// rejection at Error level, and some of them do it through an ensure, which emits a multi-line block
	// whose blank lines cannot be matched message by message. Declaring the individual messages is
	// therefore not reliable here, so error-level output is suppressed for these tests outright.
	// This only affects logs: every explicit assertion below still fails the test normally, and the
	// assertions are where this harness's actual signal lives.
	class FWireDecodeParityNoisyTest : public FAutomationTestBase
	{
	public:
		FWireDecodeParityNoisyTest(const FString& InName, const bool bInComplexTask)
			: FAutomationTestBase(InName, bInComplexTask)
		{
		}

		virtual bool SuppressLogErrors() override { return true; }
	};

	// The concrete message types each parse their own payload shape, and none of them accepts an
	// arbitrary payload. This reader takes the envelope the frame already carries and nothing else, so
	// the fixture exercises the shared header split, which is the half both implementations have to
	// agree on byte for byte.
	struct FWireDecodeParityHeaderReader final : ICrowdyMessage
	{
		virtual ECrowdyMessageType GetType() const override { return ECrowdyMessageType::GENERIC_SPATIAL_1; }
		virtual FName GetTypeName() const override { return "Wire Parity Header Reader"; }
		virtual TArray<uint8> Serialize() const override { return TArray<uint8>(); }

		[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame& Frame) override
		{
			ApplyEnvelope(Frame);
			return ApplyEnvelopeActorId(Frame);
		}
	};

	TArray<uint8> BuildWireDecodeParityDatagram(const ECrowdyMessageType Type, const TArray<uint8>& Payload,
		const int64 AppId, const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
		const ECrowdyReplicationDistance Distance, const ECrowdyDecayRate Decay,
		const int64 GameTokenId, const uint8 Sequence)
	{
		CrowdyWireParity::FParitySpatialMessage Message;
		Message.TypeOverride = Type;
		Message.AppID = AppId;
		Message.ChunkX = ChunkX;
		Message.ChunkY = ChunkY;
		Message.ChunkZ = ChunkZ;
		Message.ReplicationDistance = Distance;
		Message.DecayRate = Decay;
		Message.bContainsAuth = true;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.PayloadBytes = Payload;

		return CrowdyWireParity::AppendSignedTail(
			Message.Serialize(), CrowdyWireParity::GoldenToken(), GameTokenId, Sequence, true);
	}

	TArray<uint8> StripWireDecodeParityOpcode(const TArray<uint8>& Datagram)
	{
		if (Datagram.Num() <= WireDecodeParityOpcodeByte)
		{
			return TArray<uint8>();
		}

		return TArray<uint8>(Datagram.GetData() + WireDecodeParityOpcodeByte,
			Datagram.Num() - WireDecodeParityOpcodeByte);
	}

	TArray<uint8> BuildWireDecodeParityChannelNotification(const int64 ChannelId, const TArray<uint8>& Payload,
		const int64 EpochMillis, const uint8 Sequence)
	{
		TArray<uint8> Frame;
		Frame.Add(static_cast<uint8>(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION));
		Frame.Append(USerializationFunctionLibrary::SerializeValue<int64>(ChannelId));

		// The sender uuid is 32 ASCII octets with no terminator, so the characters go on the wire as-is.
		Frame.Append(reinterpret_cast<const uint8*>(CrowdyWireParity::GoldenUuid()),
			static_cast<int32>(crowdy::wire::kUuidSize));

		Frame.Append(USerializationFunctionLibrary::SerializeValue<uint16>(static_cast<uint16>(Payload.Num())));
		Frame.Append(Payload);
		Frame.Append(USerializationFunctionLibrary::SerializeValue<int64>(EpochMillis));
		Frame.Add(Sequence);
		return Frame;
	}

	crowdy::Bytes ViewWireDecodeParityBytes(const TArray<uint8>& Data)
	{
		return Data.Num() > 0
			? crowdy::Bytes(Data.GetData(), static_cast<std::size_t>(Data.Num()))
			: crowdy::Bytes();
	}

	// A decoded view points into the caller's buffer rather than copying, so the only meaningful
	// correctness claim about a view produced from forged bytes is that it never points outside them.
	bool WireDecodeParitySpanIsInside(const crowdy::Bytes Span, const TArray<uint8>& Buffer)
	{
		if (Span.empty())
		{
			return true;
		}

		const uint8* Begin = Buffer.GetData();
		const uint8* End = Begin + Buffer.Num();
		return Span.data() >= Begin && Span.data() + Span.size() <= End;
	}

	bool WireDecodeParityRegionIsInside(const void* Start, const int32 Length, const TArray<uint8>& Buffer)
	{
		const uint8* Begin = Buffer.GetData();
		const uint8* End = Begin + Buffer.Num();
		const uint8* RegionStart = static_cast<const uint8*>(Start);
		return RegionStart >= Begin && RegionStart + Length <= End;
	}

	TArray<uint8> MakeWireDecodeParityPattern(const int32 Length)
	{
		TArray<uint8> Pattern;
		Pattern.SetNumUninitialized(Length);
		for (int32 Index = 0; Index < Length; ++Index)
		{
			Pattern[Index] = static_cast<uint8>((Index * 7 + 13) & 0xff);
		}
		return Pattern;
	}
}

// Each implementation has to be able to read what the other writes, or a client built on one and a
// server relay built on the other agree only by accident. This is the shared codec writing and the
// Unreal message reading.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireSharedEncodeUnrealDecodeTest,
	"CrowdySDK.Wire.SharedEncodeUnrealDecode", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireSharedEncodeUnrealDecodeTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const int64 SendTimeValue = 0x0123456789abcdefLL;
	uint8 PayloadBytes[8];
	FMemory::Memcpy(PayloadBytes, &SendTimeValue, sizeof(SendTimeValue));

	LongSpatialParams Params{};
	Params.type = MessageType::GenericSpatial1;
	Params.appId = 987654321;
	Params.chunk = ChunkCoord{ 11, -22, 33 };
	Params.distance = 5;
	Params.decay = DecayRate::Linear25;
	Params.payload = crowdy::Bytes(PayloadBytes, UE_ARRAY_COUNT(PayloadBytes));
	Params.gameTokenId = 424242;
	Params.sequence = 200;
	FMemory::Memcpy(Params.uuid.data(), CrowdyWireParity::GoldenUuid(), kUuidSize);

	uint8 Buffer[kMaxDatagramSize] = {};
	const crowdy::Result<std::size_t> Written = encodeLongSpatial(
		CrowdyWireParity::Crypto(), Params, CrowdyWireParity::ParityToken(),
		crowdy::MutableBytes(Buffer, kMaxDatagramSize));

	if (!TestTrue(TEXT("shared encoder succeeded"), Written.ok()))
	{
		return false;
	}

	const TArray<uint8> Datagram(Buffer, static_cast<int32>(Written.value()));
	const TArray<uint8> Stripped = StripWireDecodeParityOpcode(Datagram);

	FPingTestMessage Decoded;
	if (!TestTrue(TEXT("Unreal decodes the shared codec's long spatial datagram"),
		CrowdyWireParity::DecodeStrippedPayload(Decoded, Stripped)))
	{
		return false;
	}

	TestEqual(TEXT("app id"), Decoded.AppID, static_cast<int64>(987654321));
	TestEqual(TEXT("chunk x"), Decoded.ChunkX, static_cast<int64>(11));
	TestEqual(TEXT("chunk y"), Decoded.ChunkY, static_cast<int64>(-22));
	TestEqual(TEXT("chunk z"), Decoded.ChunkZ, static_cast<int64>(33));
	TestEqual(TEXT("replication distance"), static_cast<int32>(Decoded.ReplicationDistance), 5);
	TestEqual(TEXT("decay rate"), static_cast<int32>(Decoded.DecayRate), 3);
	TestTrue(TEXT("contains auth"), Decoded.bContainsAuth);
	TestEqual(TEXT("uuid"), Decoded.UUID, CrowdyWireParity::GoldenActorId());
	TestEqual(TEXT("payload"), Decoded.SendTime, SendTimeValue);
	TestEqual(TEXT("game token id"), Decoded.Timestamp, static_cast<int64>(424242));
	TestEqual(TEXT("sequence"), static_cast<int32>(Decoded.SequenceNumber), 200);

	return true;
}

// The same round trip in the other direction: the Unreal message writes and the shared codec reads.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireUnrealEncodeSharedDecodeTest,
	"CrowdySDK.Wire.UnrealEncodeSharedDecode", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireUnrealEncodeSharedDecodeTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const int64 SendTimeValue = 0x0f1e2d3c4b5a6978LL;

	FPingTestMessage Message;
	Message.AppID = 5150;
	Message.ChunkX = -101;
	Message.ChunkY = 202;
	Message.ChunkZ = -303;
	Message.ReplicationDistance = ECrowdyReplicationDistance::Six_Chunks;
	Message.DecayRate = ECrowdyDecayRate::Linear_10;
	Message.bContainsAuth = true;
	Message.UUID = CrowdyWireParity::GoldenActorId();
	Message.SendTime = SendTimeValue;

	const TArray<uint8> Datagram = CrowdyWireParity::AppendSignedTail(
		Message.Serialize(), CrowdyWireParity::GoldenToken(), 777000111, 13, true);

	const crowdy::Result<LongSpatialView> Parsed = parseLongSpatial(ViewWireDecodeParityBytes(Datagram));
	if (!TestTrue(TEXT("shared codec decodes the Unreal datagram"), Parsed.ok()))
	{
		return false;
	}

	const LongSpatialView& View = Parsed.value();
	TestEqual(TEXT("type"), static_cast<int32>(View.type), 140);
	TestEqual(TEXT("app id"), View.appId, static_cast<int64>(5150));
	TestEqual(TEXT("chunk x"), View.chunk.x, static_cast<int64>(-101));
	TestEqual(TEXT("chunk y"), View.chunk.y, static_cast<int64>(202));
	TestEqual(TEXT("chunk z"), View.chunk.z, static_cast<int64>(-303));
	TestEqual(TEXT("distance"), static_cast<int32>(View.distance), 6);
	TestEqual(TEXT("decay"), static_cast<int32>(View.decay), 4);
	TestTrue(TEXT("contains auth"), View.containsAuth);
	TestTrue(TEXT("uuid"), FMemory::Memcmp(View.uuid, CrowdyWireParity::GoldenUuid(), kUuidSize) == 0);
	TestEqual(TEXT("payload size"), static_cast<int32>(View.payload.size()), 8);
	TestEqual(TEXT("game token id"), View.epochMillisOrTokenId, static_cast<int64>(777000111));
	TestEqual(TEXT("sequence"), static_cast<int32>(View.sequence), 13);

	if (View.payload.size() == sizeof(SendTimeValue) && View.payload.data() != nullptr)
	{
		int64 DecodedSendTime = 0;
		FMemory::Memcpy(&DecodedSendTime, View.payload.data(), sizeof(DecodedSendTime));
		TestEqual(TEXT("payload"), DecodedSendTime, SendTimeValue);
	}

	// The tag the Unreal transmission layer appends has to satisfy the shared verifier, or a client on
	// one implementation is rejected by a peer on the other.
	const crowdy::Status Verdict = verifyLongSpatial(CrowdyWireParity::Crypto(),
		ViewWireDecodeParityBytes(Datagram), CrowdyWireParity::ParityToken());
	TestTrue(TEXT("shared codec verifies the Unreal signature"), Verdict.ok());

	return true;
}

// Both decoders against the fixed vector. Comparing the implementations to each other cannot catch
// them drifting together; a vector derived from the published format can.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireGoldenDecodeBothSidesTest,
	"CrowdySDK.Wire.GoldenDecodeBothSides", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireGoldenDecodeBothSidesTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const TArray<uint8> Golden = CrowdyWireParity::GoldenSpatialDatagram();

	const crowdy::Result<LongSpatialView> Parsed = parseLongSpatial(ViewWireDecodeParityBytes(Golden));
	if (!TestTrue(TEXT("shared codec decodes the golden datagram"), Parsed.ok()))
	{
		return false;
	}

	const LongSpatialView& View = Parsed.value();
	TestEqual(TEXT("shared app id"), View.appId, static_cast<int64>(7));
	TestEqual(TEXT("shared chunk x"), View.chunk.x, static_cast<int64>(1));
	TestEqual(TEXT("shared chunk y"), View.chunk.y, static_cast<int64>(-2));
	TestEqual(TEXT("shared chunk z"), View.chunk.z, static_cast<int64>(3));
	TestEqual(TEXT("shared distance"), static_cast<int32>(View.distance), 8);
	TestEqual(TEXT("shared decay"), static_cast<int32>(View.decay), 1);
	TestTrue(TEXT("shared uuid"), FMemory::Memcmp(View.uuid, CrowdyWireParity::GoldenUuid(), kUuidSize) == 0);
	TestEqual(TEXT("shared payload size"), static_cast<int32>(View.payload.size()), 4);
	TestEqual(TEXT("shared game token id"), View.epochMillisOrTokenId, static_cast<int64>(123456789));
	TestEqual(TEXT("shared sequence"), static_cast<int32>(View.sequence), 42);

	if (static_cast<int32>(View.payload.size()) == 4)
	{
		const uint8 ExpectedPayload[] = { 0xde, 0xad, 0xbe, 0xef };
		TestTrue(TEXT("shared payload"), FMemory::Memcmp(View.payload.data(), ExpectedPayload, 4) == 0);
	}

	const TArray<uint8> Stripped = StripWireDecodeParityOpcode(Golden);

	FWireDecodeParityHeaderReader Reader;
	if (!TestTrue(TEXT("Unreal decodes the golden datagram header"),
		CrowdyWireParity::DecodeStrippedPayload(Reader, Stripped)))
	{
		return false;
	}

	TestEqual(TEXT("Unreal app id"), Reader.AppID, static_cast<int64>(7));
	TestEqual(TEXT("Unreal chunk x"), Reader.ChunkX, static_cast<int64>(1));
	TestEqual(TEXT("Unreal chunk y"), Reader.ChunkY, static_cast<int64>(-2));
	TestEqual(TEXT("Unreal chunk z"), Reader.ChunkZ, static_cast<int64>(3));
	TestEqual(TEXT("Unreal distance"), static_cast<int32>(Reader.ReplicationDistance), 8);
	TestEqual(TEXT("Unreal decay"), static_cast<int32>(Reader.DecayRate), 1);
	TestTrue(TEXT("Unreal contains auth"), Reader.bContainsAuth);
	TestEqual(TEXT("Unreal uuid"), Reader.UUID, CrowdyWireParity::GoldenActorId());
	TestEqual(TEXT("Unreal game token id"), Reader.Timestamp, static_cast<int64>(123456789));
	TestEqual(TEXT("Unreal sequence"), static_cast<int32>(Reader.SequenceNumber), 42);

	return true;
}

// The voxel payload is a nested layout inside the spatial payload region, so the two implementations
// have to agree twice: on where the payload starts and on how it is laid out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireVoxelPayloadRoundTripTest,
	"CrowdySDK.Wire.VoxelPayloadRoundTrip", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireVoxelPayloadRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const uint8 State[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };

	uint8 PayloadBuffer[64] = {};
	const crowdy::Result<std::size_t> Written = encodeVoxelPayload(-7, 9, -11, 1234,
		crowdy::Bytes(State, UE_ARRAY_COUNT(State)),
		crowdy::MutableBytes(PayloadBuffer, UE_ARRAY_COUNT(PayloadBuffer)));

	if (!TestTrue(TEXT("shared voxel encoder succeeded"), Written.ok()))
	{
		return false;
	}

	const TArray<uint8> SharedPayload(PayloadBuffer, static_cast<int32>(Written.value()));

	TArray<uint8> UnrealPayload;
	UnrealPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(-7));
	UnrealPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(9));
	UnrealPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(-11));
	UnrealPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(1234));
	UnrealPayload.Append(USerializationFunctionLibrary::SerializeValue<uint16>(
		static_cast<uint16>(UE_ARRAY_COUNT(State))));
	UnrealPayload.Append(State, static_cast<int32>(UE_ARRAY_COUNT(State)));

	TestTrue(FString::Printf(TEXT("both implementations write the same voxel payload bytes (%s)"),
		*CrowdyWireParity::DescribeDifference(UnrealPayload, SharedPayload)),
		UnrealPayload == SharedPayload);

	const TArray<uint8> Datagram = BuildWireDecodeParityDatagram(
		ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION, SharedPayload,
		31, 4, -5, 6, ECrowdyReplicationDistance::Three_Chunks, ECrowdyDecayRate::Linear_50, 909090, 77);

	const TArray<uint8> Stripped = StripWireDecodeParityOpcode(Datagram);

	FVoxelUpdateNotificationMessage Decoded;
	if (!TestTrue(TEXT("Unreal decodes the shared voxel payload"),
		CrowdyWireParity::DecodeStrippedPayload(Decoded, Stripped)))
	{
		return false;
	}

	TestEqual(TEXT("voxel x"), static_cast<int32>(Decoded.Vx), -7);
	TestEqual(TEXT("voxel y"), static_cast<int32>(Decoded.Vy), 9);
	TestEqual(TEXT("voxel z"), static_cast<int32>(Decoded.Vz), -11);
	TestEqual(TEXT("voxel type"), static_cast<int32>(Decoded.VoxelType), 1234);
	TestTrue(TEXT("voxel state present"), Decoded.bContainsState);
	TestEqual(TEXT("voxel state size"), static_cast<int32>(Decoded.StateSize),
		static_cast<int32>(UE_ARRAY_COUNT(State)));
	TestEqual(TEXT("voxel state length"), Decoded.StateBytes.Num(), static_cast<int32>(UE_ARRAY_COUNT(State)));

	if (Decoded.StateBytes.Num() == static_cast<int32>(UE_ARRAY_COUNT(State)))
	{
		TestTrue(TEXT("voxel state bytes"),
			FMemory::Memcmp(Decoded.StateBytes.GetData(), State, UE_ARRAY_COUNT(State)) == 0);
	}

	const crowdy::Result<LongSpatialView> Spatial = parseLongSpatial(ViewWireDecodeParityBytes(Datagram));
	if (!TestTrue(TEXT("shared codec decodes the Unreal voxel datagram"), Spatial.ok()))
	{
		return false;
	}

	const crowdy::Result<VoxelPayloadView> Voxel = parseVoxelPayload(Spatial.value().payload);
	if (!TestTrue(TEXT("shared codec decodes the voxel payload"), Voxel.ok()))
	{
		return false;
	}

	const VoxelPayloadView& VoxelView = Voxel.value();
	TestEqual(TEXT("shared voxel x"), static_cast<int32>(VoxelView.x), -7);
	TestEqual(TEXT("shared voxel y"), static_cast<int32>(VoxelView.y), 9);
	TestEqual(TEXT("shared voxel z"), static_cast<int32>(VoxelView.z), -11);
	TestEqual(TEXT("shared voxel type"), static_cast<int32>(VoxelView.voxelType), 1234);
	TestEqual(TEXT("shared voxel state size"), static_cast<int32>(VoxelView.state.size()),
		static_cast<int32>(UE_ARRAY_COUNT(State)));

	if (VoxelView.state.size() == UE_ARRAY_COUNT(State))
	{
		TestTrue(TEXT("shared voxel state bytes"),
			FMemory::Memcmp(VoxelView.state.data(), State, UE_ARRAY_COUNT(State)) == 0);
	}

	return true;
}

// The event payload is where the two implementations describe the same bytes differently, so this
// pins the overlap that does hold and the divergence that a caller has to account for.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyWireEventPayloadRoundTripTest, FWireDecodeParityNoisyTest,
	"CrowdySDK.Wire.EventPayloadRoundTrip", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireEventPayloadRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	// Arbitrary application bytes: the event state is a serialized struct whose type id will not
	// resolve here, which the decoder reports without treating the frame as malformed.


	const uint16 EventType = 0x4711;
	const uint8 State[] = { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6 };
	const int32 StateLength = static_cast<int32>(UE_ARRAY_COUNT(State));

	// The Unreal message writes a four-byte state length between the event type and the state itself.
	// The shared codec's view of an event payload is only [event type][everything else], so the length
	// prefix is part of what it hands back as state rather than something it interprets.
	TArray<uint8> LengthPrefixedState;
	LengthPrefixedState.Append(USerializationFunctionLibrary::SerializeValue<int32>(StateLength));
	LengthPrefixedState.Append(State, StateLength);

	uint8 PayloadBuffer[64] = {};
	const crowdy::Result<std::size_t> Written = encodeEventPayload(EventType,
		ViewWireDecodeParityBytes(LengthPrefixedState),
		crowdy::MutableBytes(PayloadBuffer, UE_ARRAY_COUNT(PayloadBuffer)));

	if (!TestTrue(TEXT("shared event encoder succeeded"), Written.ok()))
	{
		return false;
	}

	const TArray<uint8> SharedPayload(PayloadBuffer, static_cast<int32>(Written.value()));

	TArray<uint8> UnrealPayload;
	UnrealPayload.Append(USerializationFunctionLibrary::SerializeValue<uint16>(EventType));
	UnrealPayload.Append(LengthPrefixedState);

	TestTrue(FString::Printf(TEXT("both implementations write the same event payload bytes (%s)"),
		*CrowdyWireParity::DescribeDifference(UnrealPayload, SharedPayload)),
		UnrealPayload == SharedPayload);

	const TArray<uint8> Datagram = BuildWireDecodeParityDatagram(
		ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION, SharedPayload,
		64, -1, 2, -3, ECrowdyReplicationDistance::Two_Chunks, ECrowdyDecayRate::No_Decay, 5150, 9);

	const TArray<uint8> Stripped = StripWireDecodeParityOpcode(Datagram);

	FGameEventNotification Decoded;
	if (!TestTrue(TEXT("Unreal decodes the shared event payload"),
		CrowdyWireParity::DecodeStrippedPayload(Decoded, Stripped)))
	{
		return false;
	}

	TestEqual(TEXT("event type"), static_cast<int32>(Decoded.EventType), static_cast<int32>(EventType));
	TestEqual(TEXT("event state size"), Decoded.StateSize, StateLength);
	TestEqual(TEXT("event state length"), Decoded.StateView.Num(), StateLength);

	if (Decoded.StateView.Num() == StateLength)
	{
		TestTrue(TEXT("event state bytes"),
			FMemory::Memcmp(Decoded.StateView.GetData(), State, StateLength) == 0);
	}

	// A frame with no routing envelope must leave the routing fields at their broadcast defaults; if the
	// decoder mistook trailer bytes for an envelope, this is what would move.
	TestEqual(TEXT("event target"), static_cast<int32>(Decoded.Target),
		static_cast<int32>(ECrowdyTarget::Everyone));
	TestFalse(TEXT("event target id stays unset"), Decoded.TargetID.IsValid());

	const crowdy::Result<LongSpatialView> Spatial = parseLongSpatial(ViewWireDecodeParityBytes(Datagram));
	if (!TestTrue(TEXT("shared codec decodes the Unreal event datagram"), Spatial.ok()))
	{
		return false;
	}

	const crowdy::Result<EventPayloadView> Event = parseEventPayload(Spatial.value().payload);
	if (!TestTrue(TEXT("shared codec decodes the event payload"), Event.ok()))
	{
		return false;
	}

	const EventPayloadView& EventView = Event.value();
	TestEqual(TEXT("shared event type"), static_cast<int32>(EventView.eventType), static_cast<int32>(EventType));
	TestEqual(TEXT("shared event state size"), static_cast<int32>(EventView.state.size()),
		LengthPrefixedState.Num());

	if (static_cast<int32>(EventView.state.size()) == LengthPrefixedState.Num())
	{
		TestTrue(TEXT("shared event state is the length-prefixed block verbatim"),
			FMemory::Memcmp(EventView.state.data(), LengthPrefixedState.GetData(),
				LengthPrefixedState.Num()) == 0);
	}

	return true;
}

// These decoders consume bytes straight off a socket, so every length a sender can produce has to be
// handled, not just the well-formed ones. This walks a valid datagram down one byte at a time and
// requires each parser either to reject the frame or to return a view that stays inside the buffer it
// was given.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireMalformedSharedSweepTest,
	"CrowdySDK.Wire.MalformedSharedSweep", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireMalformedSharedSweepTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const int32 MaxSweepLength = 300;

	const TArray<uint8> Spatial = BuildWireDecodeParityDatagram(
		ECrowdyMessageType::GENERIC_SPATIAL_1, MakeWireDecodeParityPattern(200),
		1, 2, 3, 4, ECrowdyReplicationDistance::Eight_Chunks, ECrowdyDecayRate::Exponential_Decay, 8, 8);

	// Every sweep below skips the lengths its parser rejects, so a parser that rejected all of them would
	// run no checks at all and still report success. Recording that the accept path was reached at least
	// once is what keeps that from passing silently.
	int32 FirstBadSpatial = INDEX_NONE;
	bool bSawSpatialAccept = false;
	for (int32 Length = 0; Length <= FMath::Min(MaxSweepLength, Spatial.Num()); ++Length)
	{
		const TArray<uint8> Prefix(Spatial.GetData(), Length);
		const crowdy::Result<LongSpatialView> Parsed = parseLongSpatial(ViewWireDecodeParityBytes(Prefix));
		if (!Parsed.ok())
		{
			continue;
		}

		bSawSpatialAccept = true;

		const LongSpatialView& View = Parsed.value();
		const bool bConsistent =
			Length >= static_cast<int32>(kMinLongSpatialNoHmac)
			&& (!View.containsAuth || Length >= static_cast<int32>(kMinLongSpatialWithHmac))
			&& WireDecodeParitySpanIsInside(View.payload, Prefix)
			&& WireDecodeParityRegionIsInside(View.uuid, static_cast<int32>(kUuidSize), Prefix);

		if (!bConsistent)
		{
			FirstBadSpatial = Length;
			break;
		}
	}
	TestEqual(TEXT("parseLongSpatial stays self-consistent at every truncation"), FirstBadSpatial, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("parseLongSpatial accepted at least one length in the sweep"), bSawSpatialAccept);

	// Verification runs on the same untrusted bytes and must not read past them either. This one walks
	// all the way to the intact frame so the accept case is covered as well as the reject cases.
	int32 FirstBadVerify = INDEX_NONE;
	for (int32 Length = 0; Length <= Spatial.Num(); ++Length)
	{
		const TArray<uint8> Prefix(Spatial.GetData(), Length);
		const crowdy::Status Verdict = verifyLongSpatial(CrowdyWireParity::Crypto(),
			ViewWireDecodeParityBytes(Prefix), CrowdyWireParity::ParityToken());

		// Only the untruncated frame carries a tag over its own contents.
		const bool bExpectedOk = Length == Spatial.Num();
		if (Verdict.ok() != bExpectedOk)
		{
			FirstBadVerify = Length;
			break;
		}
	}
	TestEqual(TEXT("verifyLongSpatial accepts only the intact frame"), FirstBadVerify, static_cast<int32>(INDEX_NONE));

	// A real voxel payload with trailing bytes behind it. Anything shorter than the declared state has
	// to be refused, and anything longer still has to describe a state that sits inside the buffer.
	const int32 VoxelStateLength = 20;
	const TArray<uint8> VoxelState = MakeWireDecodeParityPattern(VoxelStateLength);

	uint8 VoxelBuffer[64] = {};
	const crowdy::Result<std::size_t> VoxelWritten = encodeVoxelPayload(1, 2, 3, 4,
		ViewWireDecodeParityBytes(VoxelState),
		crowdy::MutableBytes(VoxelBuffer, UE_ARRAY_COUNT(VoxelBuffer)));

	if (!TestTrue(TEXT("shared voxel encoder succeeded"), VoxelWritten.ok()))
	{
		return false;
	}

	TArray<uint8> VoxelPayload(VoxelBuffer, static_cast<int32>(VoxelWritten.value()));
	VoxelPayload.Append(MakeWireDecodeParityPattern(130));

	int32 FirstBadVoxel = INDEX_NONE;
	bool bSawVoxelAccept = false;
	for (int32 Length = 0; Length <= FMath::Min(MaxSweepLength, VoxelPayload.Num()); ++Length)
	{
		const TArray<uint8> Prefix(VoxelPayload.GetData(), Length);
		const crowdy::Result<VoxelPayloadView> Parsed = parseVoxelPayload(ViewWireDecodeParityBytes(Prefix));
		if (!Parsed.ok())
		{
			continue;
		}

		bSawVoxelAccept = true;

		if (Length < static_cast<int32>(voxel::kFixedSize) + VoxelStateLength
			|| static_cast<int32>(Parsed.value().state.size()) != VoxelStateLength
			|| !WireDecodeParitySpanIsInside(Parsed.value().state, Prefix))
		{
			FirstBadVoxel = Length;
			break;
		}
	}
	TestEqual(TEXT("parseVoxelPayload keeps its state view inside the buffer"), FirstBadVoxel, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("parseVoxelPayload accepted at least one length in the sweep"), bSawVoxelAccept);

	// The event state is a subspan of the buffer the parser was handed, so asking whether it points
	// inside that buffer can only ever answer yes. What is worth checking is that the split accounts for
	// the whole buffer and that the event type really is its first two bytes read little-endian.
	const TArray<uint8> EventPayload = MakeWireDecodeParityPattern(160);
	int32 FirstBadEvent = INDEX_NONE;
	bool bSawEventAccept = false;
	for (int32 Length = 0; Length <= FMath::Min(MaxSweepLength, EventPayload.Num()); ++Length)
	{
		const TArray<uint8> Prefix(EventPayload.GetData(), Length);
		const crowdy::Result<EventPayloadView> Parsed = parseEventPayload(ViewWireDecodeParityBytes(Prefix));
		if (!Parsed.ok())
		{
			continue;
		}

		bSawEventAccept = true;

		// The length clause has to come first: it is what keeps the two byte reads below in bounds when
		// the parser accepts a buffer too short to hold an event type.
		const EventPayloadView& View = Parsed.value();
		if (Length < static_cast<int32>(kEventTypeSize)
			|| static_cast<int32>(View.state.size()) != Length - static_cast<int32>(kEventTypeSize)
			|| View.eventType != static_cast<uint16>(Prefix[0] | (static_cast<uint16>(Prefix[1]) << 8)))
		{
			FirstBadEvent = Length;
			break;
		}
	}
	TestEqual(TEXT("parseEventPayload accounts for every byte after the event type"),
		FirstBadEvent, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("parseEventPayload accepted at least one length in the sweep"), bSawEventAccept);

	const TArray<uint8> Channel = BuildWireDecodeParityChannelNotification(
		4242, MakeWireDecodeParityPattern(120), 1700000000000LL, 5);

	int32 FirstBadChannel = INDEX_NONE;
	bool bSawChannelAccept = false;
	for (int32 Length = 0; Length <= FMath::Min(MaxSweepLength, Channel.Num()); ++Length)
	{
		const TArray<uint8> Prefix(Channel.GetData(), Length);
		const crowdy::Result<ChannelNotificationView> Parsed =
			parseChannelNotification(ViewWireDecodeParityBytes(Prefix));
		if (!Parsed.ok())
		{
			continue;
		}

		bSawChannelAccept = true;

		const ChannelNotificationView& View = Parsed.value();
		const bool bConsistent =
			Length >= static_cast<int32>(channel::kMinNotificationSize)
			&& WireDecodeParitySpanIsInside(View.payload, Prefix)
			&& WireDecodeParityRegionIsInside(View.senderUuid, static_cast<int32>(kUuidSize), Prefix);

		if (!bConsistent)
		{
			FirstBadChannel = Length;
			break;
		}
	}
	TestEqual(TEXT("parseChannelNotification stays self-consistent at every truncation"),
		FirstBadChannel, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("parseChannelNotification accepted at least one length in the sweep"), bSawChannelAccept);

	const TArray<uint8> ErrorFrame = { 3, 91, 32 };
	int32 FirstBadError = INDEX_NONE;
	for (int32 Length = 0; Length <= ErrorFrame.Num(); ++Length)
	{
		const TArray<uint8> Prefix(ErrorFrame.GetData(), Length);
		const crowdy::Result<GenericErrorView> Parsed = parseGenericError(ViewWireDecodeParityBytes(Prefix));
		const bool bExpectedOk = Length >= static_cast<int32>(kGenericErrorSize);
		if (Parsed.ok() != bExpectedOk)
		{
			FirstBadError = Length;
			break;
		}
	}
	TestEqual(TEXT("parseGenericError needs its full three bytes"), FirstBadError, static_cast<int32>(INDEX_NONE));

	int32 FirstBadReconnect = INDEX_NONE;
	const TArray<uint8> Reconnect = CrowdyWireParity::GoldenReconnectFrame();
	for (int32 Length = 0; Length <= Reconnect.Num(); ++Length)
	{
		const TArray<uint8> Prefix(Reconnect.GetData(), Length);
		const crowdy::Status Verdict = verifyCommandReconnect(CrowdyWireParity::Crypto(),
			ViewWireDecodeParityBytes(Prefix), CrowdyWireParity::ParityToken());

		const bool bExpectedOk = Length == Reconnect.Num();
		if (Verdict.ok() != bExpectedOk)
		{
			FirstBadReconnect = Length;
			break;
		}
	}
	TestEqual(TEXT("verifyCommandReconnect accepts only the intact frame"), FirstBadReconnect, static_cast<int32>(INDEX_NONE));

	// The auth flag byte is chosen by whoever sent the frame. Clearing it on an otherwise signed frame
	// makes both verifiers report success without checking anything, so a passing verdict on a frame
	// whose flag is clear says nothing about where the frame came from. This is a gap in the format as
	// both sides implement it today; the shared codec has no equivalent hole anywhere else, because
	// every other path here decides on content rather than on a sender-supplied flag.
	//
	// The observed results are recorded rather than asserted. Refusing a cleared flag would be a fix,
	// not a regression, and must not turn this test red when someone makes it.
	TArray<uint8> Unsigned = CrowdyWireParity::GoldenSpatialDatagram();
	Unsigned[static_cast<int32>(offsets::kContainsAuth)] = 0;

	const crowdy::Status UnsignedVerdict = verifyLongSpatial(CrowdyWireParity::Crypto(),
		ViewWireDecodeParityBytes(Unsigned), CrowdyWireParity::ParityToken());
	const bool bUnrealAcceptsClearedFlag =
		USerializationFunctionLibrary::AuthenticateHMAC(Unsigned, CrowdyWireParity::GoldenToken());

	AddInfo(FString::Printf(
		TEXT("cleared auth flag on a signed frame: shared verifier accepts=%s, Unreal verifier accepts=%s"),
		UnsignedVerdict.ok() ? TEXT("true") : TEXT("false"),
		bUnrealAcceptsClearedFlag ? TEXT("true") : TEXT("false")));

	// What has to hold whichever way that goes: a frame that still claims to carry a tag is only
	// acceptable when the tag matches, so corrupting one byte of it must be refused by both.
	TArray<uint8> Tampered = CrowdyWireParity::GoldenSpatialDatagram();
	const int32 TagOffset = Tampered.Num() - static_cast<int32>(kTailWithHmac);
	Tampered[TagOffset] = static_cast<uint8>(Tampered[TagOffset] ^ 0xff);

	const crowdy::Status TamperedVerdict = verifyLongSpatial(CrowdyWireParity::Crypto(),
		ViewWireDecodeParityBytes(Tampered), CrowdyWireParity::ParityToken());
	TestFalse(TEXT("shared codec refuses a frame whose tag was altered"), TamperedVerdict.ok());
	TestFalse(TEXT("Unreal refuses a frame whose tag was altered"),
		USerializationFunctionLibrary::AuthenticateHMAC(Tampered, CrowdyWireParity::GoldenToken()));

	return true;
}

// The same walk against the Unreal decoders that can be reached without a live session. Each one has
// to either refuse the frame or report a state block that fits inside the bytes it was handed.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyWireMalformedUnrealSweepTest, FWireDecodeParityNoisyTest,
	"CrowdySDK.Wire.MalformedUnrealSweep", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireMalformedUnrealSweepTest::RunTest(const FString& Parameters)
{


	const int32 MaxSweepLength = 300;

	// The message parser dereferences the game session, the service registry and the UDP subsystem for
	// several opcodes, so it cannot be driven with null dependencies. The concrete messages are what
	// actually decode the bytes, and they are reached directly here.

	// The first four payload bytes double as the state length that the actor decoder reads, so a small
	// value there keeps its accept path in the sweep rather than only its reject path.
	TArray<uint8> SpatialPayload = MakeWireDecodeParityPattern(200);
	SpatialPayload[0] = 16;
	SpatialPayload[1] = 0;
	SpatialPayload[2] = 0;
	SpatialPayload[3] = 0;

	const TArray<uint8> SpatialStripped = StripWireDecodeParityOpcode(BuildWireDecodeParityDatagram(
		ECrowdyMessageType::GENERIC_SPATIAL_1, SpatialPayload,
		1, 2, 3, 4, ECrowdyReplicationDistance::Eight_Chunks, ECrowdyDecayRate::Exponential_Decay, 8, 8));

	// Every sweep below skips the lengths its decoder rejects. A decoder that started rejecting all of
	// them would run no checks and still report success, so each sweep also records that it reached its
	// accept path at least once.
	int32 FirstBadPing = INDEX_NONE;
	bool bSawPingAccept = false;
	for (int32 Length = 0; Length <= FMath::Min(MaxSweepLength, SpatialStripped.Num()); ++Length)
	{
		const TArray<uint8> Prefix(SpatialStripped.GetData(), Length);
		FPingTestMessage Message;
		if (!CrowdyWireParity::DecodeStrippedPayload(Message, Prefix))
		{
			continue;
		}

		bSawPingAccept = true;

		if (Length < WireDecodeParityMetadataEnd + WireDecodeParityTail)
		{
			FirstBadPing = Length;
			break;
		}
	}
	TestEqual(TEXT("the generic spatial decoder needs a whole header before it accepts"),
		FirstBadPing, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("the generic spatial decoder accepted at least one length in the sweep"), bSawPingAccept);

	int32 FirstBadActor = INDEX_NONE;
	bool bSawActorAccept = false;
	for (int32 Length = 0; Length <= FMath::Min(MaxSweepLength, SpatialStripped.Num()); ++Length)
	{
		const TArray<uint8> Prefix(SpatialStripped.GetData(), Length);
		FActorUpdateNotificationMessage Message;
		if (!CrowdyWireParity::DecodeStrippedPayload(Message, Prefix))
		{
			continue;
		}

		bSawActorAccept = true;

		if (Message.StateView.Num() != Message.StateSize
			|| WireDecodeParityActorStateOffset + Message.StateView.Num() > Length)
		{
			FirstBadActor = Length;
			break;
		}
	}
	TestEqual(TEXT("the actor decoder keeps its state block inside the frame"), FirstBadActor, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("the actor decoder accepted at least one length in the sweep"), bSawActorAccept);

	TArray<uint8> EventPayload;
	EventPayload.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x1234));
	EventPayload.Append(USerializationFunctionLibrary::SerializeValue<int32>(24));
	EventPayload.Append(MakeWireDecodeParityPattern(24));

	const TArray<uint8> EventStripped = StripWireDecodeParityOpcode(BuildWireDecodeParityDatagram(
		ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION, EventPayload,
		9, 8, 7, 6, ECrowdyReplicationDistance::Four_Chunks, ECrowdyDecayRate::Linear_25, 42, 3));

	int32 FirstBadEvent = INDEX_NONE;
	bool bSawEventAccept = false;
	for (int32 Length = 0; Length <= FMath::Min(MaxSweepLength, EventStripped.Num()); ++Length)
	{
		const TArray<uint8> Prefix(EventStripped.GetData(), Length);
		FGameEventNotification Message;
		if (!CrowdyWireParity::DecodeStrippedPayload(Message, Prefix))
		{
			continue;
		}

		bSawEventAccept = true;

		if (Message.StateView.Num() != Message.StateSize
			|| WireDecodeParityEventStateOffset + Message.StateView.Num() > Length)
		{
			FirstBadEvent = Length;
			break;
		}
	}
	TestEqual(TEXT("the client event decoder keeps its state block inside the frame"),
		FirstBadEvent, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("the client event decoder accepted at least one length in the sweep"), bSawEventAccept);

	// Opcode 139 carries its own layout: the 32-byte identity region holds a source and a target GUID as
	// raw binary rather than an ASCII uuid, and the state run has no length prefix, so its end is derived
	// from the frame size instead of being read out of the frame.
	const uint16 ServerEventType = 0x5678;
	const TArray<uint8> ServerEventState = MakeWireDecodeParityPattern(40);

	TArray<uint8> ServerEventPayload;
	ServerEventPayload.Append(USerializationFunctionLibrary::SerializeValue<uint16>(ServerEventType));
	ServerEventPayload.Append(ServerEventState);

	TArray<uint8> ServerEventStripped = StripWireDecodeParityOpcode(BuildWireDecodeParityDatagram(
		ECrowdyMessageType::SERVER_EVENT_NOTIFICATION, ServerEventPayload,
		11, 12, 13, 14, ECrowdyReplicationDistance::Five_Chunks, ECrowdyDecayRate::Linear_5, 21, 4));

	// The builder writes the shared 32-byte ASCII uuid into that region because every other opcode uses
	// one. Overwriting it with two binary GUIDs is what makes this fixture the frame the decoder is
	// actually handed on the wire.
	const int32 ServerEventIdentitySize = 2 * static_cast<int32>(sizeof(FGuid));
	if (!TestTrue(TEXT("the server event fixture spans the whole identity region"),
		ServerEventStripped.Num() >= WireDecodeParityIdentityOffset + ServerEventIdentitySize))
	{
		return false;
	}

	const FGuid ServerEventSourceGuid(0x11111111, 0x22222222, 0x33333333, 0x44444444);
	const FGuid ServerEventTargetGuid(0x55555555, 0x66666666, 0x77777777, 0x88888888);
	FMemory::Memcpy(ServerEventStripped.GetData() + WireDecodeParityIdentityOffset,
		&ServerEventSourceGuid, sizeof(FGuid));
	FMemory::Memcpy(ServerEventStripped.GetData() + WireDecodeParityIdentityOffset + sizeof(FGuid),
		&ServerEventTargetGuid, sizeof(FGuid));

	int32 FirstBadServerEvent = INDEX_NONE;
	bool bSawServerEventAccept = false;
	for (int32 Length = 0; Length <= FMath::Min(MaxSweepLength, ServerEventStripped.Num()); ++Length)
	{
		const TArray<uint8> Prefix(ServerEventStripped.GetData(), Length);
		FServerEventNotification Message;
		if (!CrowdyWireParity::DecodeStrippedPayload(Message, Prefix))
		{
			continue;
		}

		bSawServerEventAccept = true;

		// Truncating this frame only ever removes bytes from the end, so whatever the decoder reports as
		// state has to be the bytes sitting at the state offset in the frame it was handed, and never
		// more of them than the frame was built with. That catches a shifted state offset, a mis-sized
		// trailer and an ignored auth flag, none of which the length arithmetic alone would show. The
		// bounds clause comes first so the comparison below cannot read past the prefix.
		const bool bStateMatchesTheFrame =
			Message.StateView.Num() <= ServerEventState.Num()
			&& (Message.StateView.Num() == 0
				|| (WireDecodeParityServerEventStateOffset + Message.StateView.Num() <= Length
					&& FMemory::Memcmp(Message.StateView.GetData(),
						Prefix.GetData() + WireDecodeParityServerEventStateOffset,
						Message.StateView.Num()) == 0));

		if (!bStateMatchesTheFrame || Message.EventType != ServerEventType)
		{
			FirstBadServerEvent = Length;
			break;
		}
	}
	TestEqual(TEXT("the server event decoder reports only state the frame actually carried"),
		FirstBadServerEvent, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("the server event decoder accepted at least one length in the sweep"), bSawServerEventAccept);

	// This sweep uses a frame that declares no state, so it covers the header and coordinate reads
	// rather than the state copy. A declared length that runs past the end of the frame is covered
	// directly by its own test rather than by truncation here.
	TArray<uint8> VoxelPayload;
	VoxelPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(1));
	VoxelPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(2));
	VoxelPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(3));
	VoxelPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(4));
	VoxelPayload.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0));

	const TArray<uint8> VoxelStripped = StripWireDecodeParityOpcode(BuildWireDecodeParityDatagram(
		ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION, VoxelPayload,
		5, 6, 7, 8, ECrowdyReplicationDistance::One_Chunk, ECrowdyDecayRate::No_Decay, 1, 1));

	int32 FirstBadVoxel = INDEX_NONE;
	bool bSawVoxelAccept = false;
	for (int32 Length = 0; Length <= FMath::Min(MaxSweepLength, VoxelStripped.Num()); ++Length)
	{
		const TArray<uint8> Prefix(VoxelStripped.GetData(), Length);
		FVoxelUpdateNotificationMessage Message;
		if (!CrowdyWireParity::DecodeStrippedPayload(Message, Prefix))
		{
			continue;
		}

		bSawVoxelAccept = true;

		if (Length < WireDecodeParityVoxelStateLenOffset + static_cast<int32>(sizeof(uint16))
			|| WireDecodeParityVoxelStateOffset + Message.StateBytes.Num() > Length)
		{
			FirstBadVoxel = Length;
			break;
		}
	}
	TestEqual(TEXT("the voxel decoder keeps its state block inside the frame"), FirstBadVoxel, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("the voxel decoder accepted at least one length in the sweep"), bSawVoxelAccept);

	// The channel notification has its own layout and is not HMAC signed, so its payload length is the
	// only thing standing between a forged frame and an over-read.
	const TArray<uint8> ChannelStripped = StripWireDecodeParityOpcode(
		BuildWireDecodeParityChannelNotification(4242, MakeWireDecodeParityPattern(120), 1700000000000LL, 5));

	int32 FirstBadChannel = INDEX_NONE;
	bool bSawChannelAccept = false;
	for (int32 Length = 0; Length <= FMath::Min(MaxSweepLength, ChannelStripped.Num()); ++Length)
	{
		const TArray<uint8> Prefix(ChannelStripped.GetData(), Length);
		FChannelMessageNotification Message;
		if (!CrowdyWireParity::DecodeStrippedPayload(Message, Prefix))
		{
			continue;
		}

		bSawChannelAccept = true;

		if (WireDecodeParityChannelPayloadOffset + Message.Payload.Num() > Length)
		{
			FirstBadChannel = Length;
			break;
		}
	}
	TestEqual(TEXT("the channel decoder keeps its payload inside the frame"), FirstBadChannel, static_cast<int32>(INDEX_NONE));
	TestTrue(TEXT("the channel decoder accepted at least one length in the sweep"), bSawChannelAccept);

	return true;
}

// A forged inner state length is the one field of the voxel layout that a sender fully controls and
// that the receiver turns straight into a copy length.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyWireForgedVoxelStateLengthTest, FWireDecodeParityNoisyTest,
	"CrowdySDK.Wire.ForgedVoxelStateLength", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireForgedVoxelStateLengthTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;



	const uint8 State[] = { 0x11, 0x22, 0x33, 0x44, 0x55 };

	TArray<uint8> ForgedPayload;
	ForgedPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(1));
	ForgedPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(2));
	ForgedPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(3));
	ForgedPayload.Append(USerializationFunctionLibrary::SerializeValue<int16>(4));
	ForgedPayload.Append(USerializationFunctionLibrary::SerializeValue<uint16>(60000));
	ForgedPayload.Append(State, static_cast<int32>(UE_ARRAY_COUNT(State)));

	const TArray<uint8> Datagram = BuildWireDecodeParityDatagram(
		ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION, ForgedPayload,
		1, 1, 1, 1, ECrowdyReplicationDistance::One_Chunk, ECrowdyDecayRate::No_Decay, 1, 1);

	const TArray<uint8> Stripped = StripWireDecodeParityOpcode(Datagram);

	// The shared codec compares the declared length against the bytes present and rejects the frame.
	// This is the bound the Unreal decoder should apply as well.
	const crowdy::Result<LongSpatialView> Spatial = parseLongSpatial(ViewWireDecodeParityBytes(Datagram));
	if (!TestTrue(TEXT("shared codec decodes the forged datagram's header"), Spatial.ok()))
	{
		return false;
	}

	const crowdy::Result<VoxelPayloadView> Voxel = parseVoxelPayload(Spatial.value().payload);
	TestFalse(TEXT("shared codec rejects a declared state length past the end of the payload"), Voxel.ok());

	// Both implementations have to refuse a declared length that runs past the bytes the frame carries,
	// or the length is used to size a copy out of memory the frame never owned.
	FVoxelUpdateNotificationMessage Decoded;

	TestFalse(TEXT("the Unreal decoder refuses a declared state length past the end of the frame"),
		CrowdyWireParity::DecodeStrippedPayload(Decoded, Stripped));
	TestFalse(TEXT("no state is reported for an over-long declared length"), Decoded.bContainsState);
	TestEqual(TEXT("no state bytes are copied for an over-long declared length"), Decoded.StateBytes.Num(), 0);

	return true;
}

// A declared state length below the decoder's fixed ceiling but still past the bytes the frame carries.
// The ceiling alone does not make this safe, so the length has to be checked against the frame, while a
// frame whose length really does match its state still has to decode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireVoxelForgedStateLengthPastFrameTest,
	"CrowdySDK.Wire.VoxelForgedStateLengthPastFrame", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireVoxelForgedStateLengthPastFrameTest::RunTest(const FString& Parameters)
{
	const uint8 State[] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee };
	const int32 StateLength = static_cast<int32>(UE_ARRAY_COUNT(State));

	TArray<uint8> Payload;
	Payload.Append(USerializationFunctionLibrary::SerializeValue<int16>(1));
	Payload.Append(USerializationFunctionLibrary::SerializeValue<int16>(2));
	Payload.Append(USerializationFunctionLibrary::SerializeValue<int16>(3));
	Payload.Append(USerializationFunctionLibrary::SerializeValue<int16>(4));
	Payload.Append(USerializationFunctionLibrary::SerializeValue<uint16>(static_cast<uint16>(StateLength)));
	Payload.Append(State, StateLength);

	const TArray<uint8> Datagram = BuildWireDecodeParityDatagram(
		ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION, Payload,
		1, 1, 1, 1, ECrowdyReplicationDistance::One_Chunk, ECrowdyDecayRate::No_Decay, 1, 1);
	const TArray<uint8> Stripped = StripWireDecodeParityOpcode(Datagram);

	TArray<uint8> Tampered = Stripped;
	const uint16 ForgedLength = 4000;
	const TArray<uint8> ForgedLengthBytes = USerializationFunctionLibrary::SerializeValue<uint16>(ForgedLength);
	FMemory::Memcpy(Tampered.GetData() + WireDecodeParityVoxelStateLenOffset, ForgedLengthBytes.GetData(),
		ForgedLengthBytes.Num());

	if (!TestTrue(TEXT("the forged length stays under the decoder's fixed ceiling"),
		static_cast<int32>(ForgedLength) < 5000))
	{
		return false;
	}
	if (!TestTrue(TEXT("the forged length still runs past the end of the frame"),
		WireDecodeParityVoxelStateOffset + static_cast<int32>(ForgedLength) > Tampered.Num()))
	{
		return false;
	}

	FVoxelUpdateNotificationMessage Forged;
	TestFalse(TEXT("the voxel decoder refuses a length under the ceiling that runs past the frame"),
		CrowdyWireParity::DecodeStrippedPayload(Forged, Tampered));

	FVoxelUpdateNotificationMessage WellFormed;
	if (!TestTrue(TEXT("the voxel decoder still accepts the untampered frame"),
		CrowdyWireParity::DecodeStrippedPayload(WellFormed, Stripped)))
	{
		return false;
	}

	TestEqual(TEXT("well-formed voxel state length"), WellFormed.StateBytes.Num(), StateLength);
	if (WellFormed.StateBytes.Num() == StateLength)
	{
		TestTrue(TEXT("well-formed voxel state bytes"),
			FMemory::Memcmp(WellFormed.StateBytes.GetData(), State, StateLength) == 0);
	}

	return true;
}

// The actor decoder's bound is written as a subtraction rather than an addition specifically because a
// large declared length would overflow an addition. This proves the overflow shape itself is refused,
// not just an ordinary over-long length.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireActorForgedStateLengthOverflowTest,
	"CrowdySDK.Wire.ActorForgedStateLengthOverflow", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireActorForgedStateLengthOverflowTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> State = MakeWireDecodeParityPattern(8);

	TArray<uint8> Payload;
	Payload.Append(USerializationFunctionLibrary::SerializeValue<int32>(State.Num()));
	Payload.Append(State);

	const TArray<uint8> Datagram = BuildWireDecodeParityDatagram(
		ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION, Payload,
		1, 1, 1, 1, ECrowdyReplicationDistance::One_Chunk, ECrowdyDecayRate::No_Decay, 1, 1);
	const TArray<uint8> Stripped = StripWireDecodeParityOpcode(Datagram);

	// The state length field sits right after the shared metadata block, one int32 before the state
	// bytes this decoder's own offset constant points at.
	const int32 StateLenOffset = WireDecodeParityActorStateOffset - static_cast<int32>(sizeof(int32));

	TArray<uint8> Tampered = Stripped;
	const TArray<uint8> ForgedLengthBytes = USerializationFunctionLibrary::SerializeValue<int32>(MAX_int32);
	FMemory::Memcpy(Tampered.GetData() + StateLenOffset, ForgedLengthBytes.GetData(), ForgedLengthBytes.Num());

	FActorUpdateNotificationMessage Forged;
	TestFalse(TEXT("the actor decoder refuses a declared length that would overflow the bound"),
		CrowdyWireParity::DecodeStrippedPayload(Forged, Tampered));
	TestEqual(TEXT("no state bytes are copied for the overflowing declared length"),
		Forged.StateView.Num(), 0);

	FActorUpdateNotificationMessage WellFormed;
	if (!TestTrue(TEXT("the actor decoder still accepts the untampered frame"),
		CrowdyWireParity::DecodeStrippedPayload(WellFormed, Stripped)))
	{
		return false;
	}

	TestEqual(TEXT("well-formed actor state length"), WellFormed.StateView.Num(), State.Num());
	if (WellFormed.StateView.Num() == State.Num())
	{
		TestTrue(TEXT("well-formed actor state bytes"),
			FMemory::Memcmp(WellFormed.StateView.GetData(), State.GetData(), State.Num()) == 0);
	}

	return true;
}

// The audio frame loop reads a per-frame size straight off the wire and, like the actor state length,
// bounds it with a subtraction rather than an addition so a huge declared size cannot wrap the check.
// FClientAudioNotification has three fields ahead of the frame count (sample rate, channel count, then
// the count itself), so the forged frame size sits after all three.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireAudioForgedFrameSizeOverflowTest,
	"CrowdySDK.Wire.AudioForgedFrameSizeOverflow", CrowdyWireDecodeParityTestFlags)
bool FCrowdyWireAudioForgedFrameSizeOverflowTest::RunTest(const FString& Parameters)
{
	TArray<uint8> ForgedPayload;
	ForgedPayload.Append(USerializationFunctionLibrary::SerializeValue<int32>(48000)); // sample rate
	ForgedPayload.Append(USerializationFunctionLibrary::SerializeValue<int32>(2));     // channel count
	ForgedPayload.Append(USerializationFunctionLibrary::SerializeValue<int32>(1));     // frame count
	ForgedPayload.Append(USerializationFunctionLibrary::SerializeValue<int32>(MAX_int32)); // forged frame size

	const TArray<uint8> ForgedDatagram = BuildWireDecodeParityDatagram(
		ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION, ForgedPayload,
		1, 1, 1, 1, ECrowdyReplicationDistance::One_Chunk, ECrowdyDecayRate::No_Decay, 1, 1);
	const TArray<uint8> ForgedStripped = StripWireDecodeParityOpcode(ForgedDatagram);

	FClientAudioNotification Forged;
	TestFalse(TEXT("the audio decoder refuses a declared frame size that would overflow the bound"),
		CrowdyWireParity::DecodeStrippedPayload(Forged, ForgedStripped));

	const TArray<uint8> FrameBytes = MakeWireDecodeParityPattern(6);

	TArray<uint8> WellFormedPayload;
	WellFormedPayload.Append(USerializationFunctionLibrary::SerializeValue<int32>(48000));
	WellFormedPayload.Append(USerializationFunctionLibrary::SerializeValue<int32>(2));
	WellFormedPayload.Append(USerializationFunctionLibrary::SerializeValue<int32>(1));
	WellFormedPayload.Append(USerializationFunctionLibrary::SerializeValue<int32>(FrameBytes.Num()));
	WellFormedPayload.Append(FrameBytes);

	const TArray<uint8> WellFormedDatagram = BuildWireDecodeParityDatagram(
		ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION, WellFormedPayload,
		1, 1, 1, 1, ECrowdyReplicationDistance::One_Chunk, ECrowdyDecayRate::No_Decay, 1, 1);
	const TArray<uint8> WellFormedStripped = StripWireDecodeParityOpcode(WellFormedDatagram);

	FClientAudioNotification WellFormed;
	if (!TestTrue(TEXT("the audio decoder still accepts a well-formed single frame"),
		CrowdyWireParity::DecodeStrippedPayload(WellFormed, WellFormedStripped)))
	{
		return false;
	}

	TestEqual(TEXT("well-formed frame count"), WellFormed.Frames.Num(), 1);
	if (WellFormed.Frames.Num() == 1)
	{
		TestEqual(TEXT("well-formed frame size"), WellFormed.Frames[0].FrameSize, FrameBytes.Num());
		TestEqual(TEXT("well-formed frame bytes"), WellFormed.Frames[0].AudioData.Num(), FrameBytes.Num());
		if (WellFormed.Frames[0].AudioData.Num() == FrameBytes.Num())
		{
			TestTrue(TEXT("well-formed frame contents"),
				FMemory::Memcmp(WellFormed.Frames[0].AudioData.GetData(), FrameBytes.GetData(),
					FrameBytes.Num()) == 0);
		}
	}

	return true;
}

#endif
