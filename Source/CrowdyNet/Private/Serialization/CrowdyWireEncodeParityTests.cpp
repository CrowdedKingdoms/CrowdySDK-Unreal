#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CrowdyWireParitySupport.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Messages/GameObjects/FGameEventRequest.h"
#include "Messages/Voxel/FVoxelStateUpdateRequest.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyWireEncodeParityTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The engine plugin and the shared C++ SDK each build their own datagram bytes. Anything that reaches
// the socket from one has to be indistinguishable from the other, so these tests build the same
// logical message twice and compare the buffers byte for byte.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireLongSpatialOpcodeParityTest,
	"CrowdySDK.Wire.LongSpatialOpcodeParity", CrowdyWireEncodeParityTestFlags)
bool FCrowdyWireLongSpatialOpcodeParityTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const TArray<uint8> Payload = { 0x01, 0x7f, 0x80, 0xff, 0x2a, 0x00, 0x11 };

	const ECrowdyMessageType Opcodes[] = {
		ECrowdyMessageType::ACTOR_UPDATE_REQUEST,
		ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION,
		ECrowdyMessageType::SERVER_EVENT_NOTIFICATION,
		ECrowdyMessageType::GENERIC_SPATIAL_1,
		ECrowdyMessageType::SINGLE_ACTOR_MESSAGE
	};

	// The loop below skips any opcode the shared encoder turns down, so an encoder that turned down all
	// of them would compare nothing and still report success. Counting the comparisons that actually ran
	// is what keeps that from passing silently.
	int32 ComparedOpcodes = 0;

	for (const ECrowdyMessageType Opcode : Opcodes)
	{
		CrowdyWireParity::FParitySpatialMessage Message;
		Message.TypeOverride = Opcode;
		Message.AppID = 900719925474099;
		Message.ChunkX = 12;
		Message.ChunkY = -4096;
		Message.ChunkZ = 7;
		Message.ReplicationDistance = ECrowdyReplicationDistance::Six_Chunks;
		Message.DecayRate = ECrowdyDecayRate::Linear_25;
		Message.bContainsAuth = true;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.PayloadBytes = Payload;

		const TArray<uint8> UnrealDatagram = CrowdyWireParity::AppendSignedTail(
			Message.Serialize(), CrowdyWireParity::GoldenToken(), -98765432101234, 200, true);

		LongSpatialParams Params{};
		Params.type = static_cast<MessageType>(static_cast<uint8>(Opcode));
		Params.appId = 900719925474099;
		Params.chunk = ChunkCoord{ 12, -4096, 7 };
		Params.distance = 6;
		Params.decay = DecayRate::Linear25;
		Params.payload = crowdy::Bytes(Payload.GetData(), static_cast<std::size_t>(Payload.Num()));
		Params.gameTokenId = -98765432101234;
		Params.sequence = 200;
		FMemory::Memcpy(Params.uuid.data(), CrowdyWireParity::GoldenUuid(), kUuidSize);

		uint8 Buffer[kMaxDatagramSize] = {};
		const crowdy::Result<std::size_t> Written = encodeLongSpatial(
			CrowdyWireParity::Crypto(), Params, CrowdyWireParity::ParityToken(),
			crowdy::MutableBytes(Buffer, kMaxDatagramSize));

		if (!TestTrue(FString::Printf(TEXT("shared encoder accepted opcode %d"),
			static_cast<int32>(Opcode)), Written.ok()))
		{
			continue;
		}

		const TArray<uint8> SharedDatagram(Buffer, static_cast<int32>(Written.value()));

		++ComparedOpcodes;

		TestTrue(FString::Printf(TEXT("opcode %d encodes identically (%s)"), static_cast<int32>(Opcode),
			*CrowdyWireParity::DescribeDifference(UnrealDatagram, SharedDatagram)),
			UnrealDatagram == SharedDatagram);
	}

	TestEqual(TEXT("every opcode reached the byte-for-byte comparison"), ComparedOpcodes,
		static_cast<int32>(UE_ARRAY_COUNT(Opcodes)));

	return true;
}

// A message with no payload is the shortest legal signed spatial frame, and an off-by-one in either
// header or tail arithmetic shows up here before it shows up anywhere else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireEmptyPayloadParityTest,
	"CrowdySDK.Wire.EmptyPayloadParity", CrowdyWireEncodeParityTestFlags)
bool FCrowdyWireEmptyPayloadParityTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	CrowdyWireParity::FParitySpatialMessage Message;
	Message.TypeOverride = ECrowdyMessageType::GENERIC_SPATIAL_1;
	Message.AppID = 3;
	Message.ChunkX = -1;
	Message.ChunkY = 0;
	Message.ChunkZ = 1;
	Message.ReplicationDistance = ECrowdyReplicationDistance::One_Chunk;
	Message.DecayRate = ECrowdyDecayRate::No_Decay;
	Message.bContainsAuth = true;
	Message.UUID = CrowdyWireParity::GoldenActorId();

	const TArray<uint8> UnrealDatagram = CrowdyWireParity::AppendSignedTail(
		Message.Serialize(), CrowdyWireParity::GoldenToken(), 1, 0, true);

	LongSpatialParams Params{};
	Params.type = MessageType::GenericSpatial1;
	Params.appId = 3;
	Params.chunk = ChunkCoord{ -1, 0, 1 };
	Params.distance = 1;
	Params.decay = DecayRate::None;
	Params.gameTokenId = 1;
	Params.sequence = 0;
	FMemory::Memcpy(Params.uuid.data(), CrowdyWireParity::GoldenUuid(), kUuidSize);

	uint8 Buffer[kMaxDatagramSize] = {};
	const crowdy::Result<std::size_t> Written = encodeLongSpatial(
		CrowdyWireParity::Crypto(), Params, CrowdyWireParity::ParityToken(),
		crowdy::MutableBytes(Buffer, kMaxDatagramSize));

	if (!TestTrue(TEXT("shared encoder succeeded"), Written.ok()))
	{
		return false;
	}

	const TArray<uint8> SharedDatagram(Buffer, static_cast<int32>(Written.value()));

	TestEqual(TEXT("empty-payload datagram length"), UnrealDatagram.Num(),
		static_cast<int32>(kMinLongSpatialWithHmac));
	TestTrue(FString::Printf(TEXT("empty payload encodes identically (%s)"),
		*CrowdyWireParity::DescribeDifference(UnrealDatagram, SharedDatagram)),
		UnrealDatagram == SharedDatagram);

	return true;
}

// The largest payload that still fits one datagram, plus one byte more. The shared encoder refuses the
// oversized message. The engine-side message structs apply no size cap of their own: neither
// SerializeMetadata nor the concrete message structs check a length, so an oversized payload is
// serialized as-is and only fails later at the socket. That asymmetry is deliberate here, so the
// oversized case only asserts the shared encoder's refusal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireMaxPayloadParityTest,
	"CrowdySDK.Wire.MaxPayloadParity", CrowdyWireEncodeParityTestFlags)
bool FCrowdyWireMaxPayloadParityTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	TArray<uint8> Payload;
	Payload.SetNumUninitialized(static_cast<int32>(kMaxLongSpatialPayload));
	for (int32 Index = 0; Index < Payload.Num(); ++Index)
	{
		Payload[Index] = static_cast<uint8>(Index * 7 + 3);
	}

	CrowdyWireParity::FParitySpatialMessage Message;
	Message.TypeOverride = ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
	Message.AppID = 42;
	Message.ChunkX = 5;
	Message.ChunkY = -6;
	Message.ChunkZ = 7;
	Message.ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks;
	Message.DecayRate = ECrowdyDecayRate::Exponential_Decay;
	Message.bContainsAuth = true;
	Message.UUID = CrowdyWireParity::GoldenActorId();
	Message.PayloadBytes = Payload;

	const TArray<uint8> UnrealDatagram = CrowdyWireParity::AppendSignedTail(
		Message.Serialize(), CrowdyWireParity::GoldenToken(), 77, 3, true);

	LongSpatialParams Params{};
	Params.type = MessageType::ClientEventNotification;
	Params.appId = 42;
	Params.chunk = ChunkCoord{ 5, -6, 7 };
	Params.distance = 8;
	Params.decay = DecayRate::Exponential;
	Params.payload = crowdy::Bytes(Payload.GetData(), static_cast<std::size_t>(Payload.Num()));
	Params.gameTokenId = 77;
	Params.sequence = 3;
	FMemory::Memcpy(Params.uuid.data(), CrowdyWireParity::GoldenUuid(), kUuidSize);

	uint8 Buffer[kMaxDatagramSize] = {};
	const crowdy::Result<std::size_t> Written = encodeLongSpatial(
		CrowdyWireParity::Crypto(), Params, CrowdyWireParity::ParityToken(),
		crowdy::MutableBytes(Buffer, kMaxDatagramSize));

	if (!TestTrue(TEXT("shared encoder accepted the maximum payload"), Written.ok()))
	{
		return false;
	}

	const TArray<uint8> SharedDatagram(Buffer, static_cast<int32>(Written.value()));

	TestEqual(TEXT("maximum datagram fills the size budget exactly"), SharedDatagram.Num(),
		static_cast<int32>(kMaxDatagramSize));
	TestTrue(FString::Printf(TEXT("maximum payload encodes identically (%s)"),
		*CrowdyWireParity::DescribeDifference(UnrealDatagram, SharedDatagram)),
		UnrealDatagram == SharedDatagram);

	TArray<uint8> Oversized = Payload;
	Oversized.Add(0xff);

	LongSpatialParams OversizedParams = Params;
	OversizedParams.payload = crowdy::Bytes(Oversized.GetData(), static_cast<std::size_t>(Oversized.Num()));

	uint8 OversizedBuffer[kMaxDatagramSize + 64] = {};
	const crowdy::Result<std::size_t> OversizedWritten = encodeLongSpatial(
		CrowdyWireParity::Crypto(), OversizedParams, CrowdyWireParity::ParityToken(),
		crowdy::MutableBytes(OversizedBuffer, UE_ARRAY_COUNT(OversizedBuffer)));

	TestFalse(TEXT("shared encoder refuses one byte over the maximum payload"), OversizedWritten.ok());
	TestTrue(TEXT("oversized payload is rejected as an invalid argument"),
		OversizedWritten.error() == crowdy::Errc::InvalidArgument);

	return true;
}

// The shared encoder always marks a long spatial message as authenticated, because it always signs
// one. The engine-side message carries the flag as a member and can clear it, in which case the
// transmission layer also omits the tag. Parity therefore only holds for signed messages; this pins
// the unsigned shape so the difference is visible rather than surprising.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireContainsAuthDivergenceTest,
	"CrowdySDK.Wire.ContainsAuthDivergence", CrowdyWireEncodeParityTestFlags)
bool FCrowdyWireContainsAuthDivergenceTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const TArray<uint8> Payload = { 0xaa, 0xbb };

	CrowdyWireParity::FParitySpatialMessage Message;
	Message.TypeOverride = ECrowdyMessageType::GENERIC_SPATIAL_1;
	Message.AppID = 9;
	Message.ChunkX = 1;
	Message.ChunkY = -2;
	Message.ChunkZ = 3;
	Message.ReplicationDistance = ECrowdyReplicationDistance::Two_Chunks;
	Message.DecayRate = ECrowdyDecayRate::Linear_50;
	Message.bContainsAuth = false;
	Message.UUID = CrowdyWireParity::GoldenActorId();
	Message.PayloadBytes = Payload;

	const TArray<uint8> UnsignedDatagram = CrowdyWireParity::AppendSignedTail(
		Message.Serialize(), CrowdyWireParity::GoldenToken(), 5, 1, false);

	TestEqual(TEXT("unsigned datagram clears the containsAuth byte"),
		static_cast<int32>(UnsignedDatagram[offsets::kContainsAuth]), 0);
	TestEqual(TEXT("unsigned datagram omits the tag"), UnsignedDatagram.Num(),
		static_cast<int32>(kLongSpatialHeaderSize + Payload.Num() + kTailNoHmac));

	LongSpatialParams Params{};
	Params.type = MessageType::GenericSpatial1;
	Params.appId = 9;
	Params.chunk = ChunkCoord{ 1, -2, 3 };
	Params.distance = 2;
	Params.decay = DecayRate::Linear50;
	Params.payload = crowdy::Bytes(Payload.GetData(), static_cast<std::size_t>(Payload.Num()));
	Params.gameTokenId = 5;
	Params.sequence = 1;
	FMemory::Memcpy(Params.uuid.data(), CrowdyWireParity::GoldenUuid(), kUuidSize);

	uint8 Buffer[kMaxDatagramSize] = {};
	const crowdy::Result<std::size_t> Written = encodeLongSpatial(
		CrowdyWireParity::Crypto(), Params, CrowdyWireParity::ParityToken(),
		crowdy::MutableBytes(Buffer, kMaxDatagramSize));

	if (!TestTrue(TEXT("shared encoder succeeded"), Written.ok()))
	{
		return false;
	}

	const TArray<uint8> SignedDatagram(Buffer, static_cast<int32>(Written.value()));

	TestEqual(TEXT("shared encoder always sets the containsAuth byte"),
		static_cast<int32>(SignedDatagram[offsets::kContainsAuth]), 1);

	// Comparing the two buffers outright only ever reports that they differ, since one is 32 bytes
	// longer. What is worth knowing is that the flag byte and the tag are the only things it changes, so
	// the unsigned form is rebuilt out of the signed one instead: that catches a header field or a
	// trailer that also moved when the flag was cleared.
	TArray<uint8> ExpectedUnsigned = SignedDatagram;
	ExpectedUnsigned[static_cast<int32>(offsets::kContainsAuth)] = 0;
	ExpectedUnsigned.RemoveAt(static_cast<int32>(kLongSpatialHeaderSize) + Payload.Num(),
		static_cast<int32>(kHmacTagSize));

	TestTrue(FString::Printf(TEXT("clearing the flag changes only the flag byte and the tag (%s)"),
		*CrowdyWireParity::DescribeDifference(UnsignedDatagram, ExpectedUnsigned)),
		UnsignedDatagram == ExpectedUnsigned);

	// The same message with the flag set is byte-identical, which is what the send path always
	// produces today.
	Message.bContainsAuth = true;
	const TArray<uint8> ResignedDatagram = CrowdyWireParity::AppendSignedTail(
		Message.Serialize(), CrowdyWireParity::GoldenToken(), 5, 1, true);

	TestTrue(FString::Printf(TEXT("the signed form encodes identically (%s)"),
		*CrowdyWireParity::DescribeDifference(ResignedDatagram, SignedDatagram)),
		ResignedDatagram == SignedDatagram);

	return true;
}

// Channel publishes use a shorter layout of their own, with the containsAuth flag after the payload
// rather than in a fixed header slot, so the signed prefix is a different length from a spatial
// message's.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireChannelRequestParityTest,
	"CrowdySDK.Wire.ChannelRequestParity", CrowdyWireEncodeParityTestFlags)
bool FCrowdyWireChannelRequestParityTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	TArray<uint8> Payload;
	Payload.SetNumUninitialized(300);
	for (int32 Index = 0; Index < Payload.Num(); ++Index)
	{
		Payload[Index] = static_cast<uint8>(255 - (Index % 251));
	}

	FChannelMessageRequest Request;
	Request.ChannelId = -1234567890123;
	Request.UUID = CrowdyWireParity::GoldenActorId();
	Request.Payload = Payload;

	const TArray<uint8> UnrealDatagram = CrowdyWireParity::AppendSignedTail(
		Request.Serialize(), CrowdyWireParity::GoldenToken(), 55555, 9, true);

	ChannelMessageParams Params{};
	Params.channelId = -1234567890123;
	Params.payload = crowdy::Bytes(Payload.GetData(), static_cast<std::size_t>(Payload.Num()));
	Params.gameTokenId = 55555;
	Params.sequence = 9;
	FMemory::Memcpy(Params.uuid.data(), CrowdyWireParity::GoldenUuid(), kUuidSize);

	uint8 Buffer[kMaxDatagramSize] = {};
	const crowdy::Result<std::size_t> Written = encodeChannelMessage(
		CrowdyWireParity::Crypto(), Params, CrowdyWireParity::ParityToken(),
		crowdy::MutableBytes(Buffer, kMaxDatagramSize));

	if (!TestTrue(TEXT("shared encoder succeeded"), Written.ok()))
	{
		return false;
	}

	const TArray<uint8> SharedDatagram(Buffer, static_cast<int32>(Written.value()));

	TestEqual(TEXT("channel request length"), UnrealDatagram.Num(),
		static_cast<int32>(channelRequestSize(static_cast<std::size_t>(Payload.Num()))));
	TestTrue(FString::Printf(TEXT("channel request encodes identically (%s)"),
		*CrowdyWireParity::DescribeDifference(UnrealDatagram, SharedDatagram)),
		UnrealDatagram == SharedDatagram);

	// The shared encoder caps a channel payload at the server's limit. The engine-side message has no
	// cap and writes the length into two bytes, so a payload past the limit is accepted here and only
	// rejected server-side.
	TArray<uint8> Oversized;
	Oversized.SetNumZeroed(static_cast<int32>(channel::kMaxPayload) + 1);

	ChannelMessageParams OversizedParams = Params;
	OversizedParams.payload = crowdy::Bytes(Oversized.GetData(), static_cast<std::size_t>(Oversized.Num()));

	uint8 OversizedBuffer[kMaxDatagramSize + 64] = {};
	const crowdy::Result<std::size_t> OversizedWritten = encodeChannelMessage(
		CrowdyWireParity::Crypto(), OversizedParams, CrowdyWireParity::ParityToken(),
		crowdy::MutableBytes(OversizedBuffer, UE_ARRAY_COUNT(OversizedBuffer)));

	TestFalse(TEXT("shared encoder refuses an oversized channel payload"), OversizedWritten.ok());

	return true;
}

// A voxel update carries its own payload inside the spatial layout. Comparing only the payload region
// keeps this test about the voxel framing rather than re-testing the spatial header.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireVoxelPayloadParityTest,
	"CrowdySDK.Wire.VoxelPayloadParity", CrowdyWireEncodeParityTestFlags)
bool FCrowdyWireVoxelPayloadParityTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const TArray<uint8> State = { 0x10, 0x20, 0x30, 0x40, 0x50 };

	FVoxelStateUpdateRequest Request;
	Request.UUID = CrowdyWireParity::GoldenActorId();
	Request.AppID = 11;
	Request.ChunkX = 2;
	Request.ChunkY = -3;
	Request.ChunkZ = 4;
	Request.Vx = -17;
	Request.Vy = 300;
	Request.Vz = 31;
	Request.VoxelType = -2;
	Request.StateBytes = State;
	Request.StateSize = static_cast<uint16>(State.Num());
	Request.bContainsState = true;

	const TArray<uint8> Serialized = Request.Serialize();
	if (!TestTrue(TEXT("serialized voxel request carries a payload"),
		Serialized.Num() > static_cast<int32>(kLongSpatialHeaderSize)))
	{
		return false;
	}

	const TArray<uint8> UnrealPayload(Serialized.GetData() + kLongSpatialHeaderSize,
		Serialized.Num() - static_cast<int32>(kLongSpatialHeaderSize));

	uint8 Buffer[voxel::kFixedSize + 64] = {};
	const crowdy::Result<std::size_t> Written = encodeVoxelPayload(
		static_cast<std::int16_t>(-17), static_cast<std::int16_t>(300),
		static_cast<std::int16_t>(31), static_cast<std::int16_t>(-2),
		crowdy::Bytes(State.GetData(), static_cast<std::size_t>(State.Num())),
		crowdy::MutableBytes(Buffer, UE_ARRAY_COUNT(Buffer)));

	if (!TestTrue(TEXT("shared voxel encoder succeeded"), Written.ok()))
	{
		return false;
	}

	const TArray<uint8> SharedPayload(Buffer, static_cast<int32>(Written.value()));

	TestEqual(TEXT("voxel payload length"), UnrealPayload.Num(),
		static_cast<int32>(voxel::kFixedSize) + State.Num());
	TestTrue(FString::Printf(TEXT("voxel payload encodes identically (%s)"),
		*CrowdyWireParity::DescribeDifference(UnrealPayload, SharedPayload)),
		UnrealPayload == SharedPayload);

	// The shared encoder derives the advertised state length from the state it is given, so the two
	// can never disagree. The engine-side message keeps the length in a separate field and skips the
	// state bytes unless a flag is set, so it can emit a length with no bytes behind it. Nothing in
	// the shared encoder can produce that frame, and a reader will treat it as truncated.
	FVoxelStateUpdateRequest Inconsistent;
	Inconsistent.UUID = CrowdyWireParity::GoldenActorId();
	Inconsistent.AppID = 11;
	Inconsistent.ChunkX = 2;
	Inconsistent.ChunkY = -3;
	Inconsistent.ChunkZ = 4;
	Inconsistent.Vx = -17;
	Inconsistent.Vy = 300;
	Inconsistent.Vz = 31;
	Inconsistent.VoxelType = -2;
	Inconsistent.StateBytes = State;
	Inconsistent.StateSize = static_cast<uint16>(State.Num());
	Inconsistent.bContainsState = false;

	const TArray<uint8> InconsistentSerialized = Inconsistent.Serialize();
	const TArray<uint8> InconsistentPayload(InconsistentSerialized.GetData() + kLongSpatialHeaderSize,
		InconsistentSerialized.Num() - static_cast<int32>(kLongSpatialHeaderSize));

	TestEqual(TEXT("a suppressed state leaves only the fixed voxel header"), InconsistentPayload.Num(),
		static_cast<int32>(voxel::kFixedSize));
	TestEqual(TEXT("the advertised state length still reports the missing bytes"),
		static_cast<int32>(crowdy::le::readU16(InconsistentPayload.GetData() + voxel::kStateLenOffset)),
		State.Num());
	TestFalse(TEXT("the shared encoder cannot produce a length with no state behind it"),
		InconsistentPayload == SharedPayload);

	return true;
}

// Game events carry an event type and a state blob in the spatial payload region. The shared codec
// models that region as an event type followed by opaque bytes; the engine-side message gives those
// bytes further structure, which this test spells out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireEventPayloadParityTest,
	"CrowdySDK.Wire.EventPayloadParity", CrowdyWireEncodeParityTestFlags)
bool FCrowdyWireEventPayloadParityTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const TArray<uint8> State = { 0xca, 0xfe, 0xba, 0xbe, 0x01, 0x02 };

	FGameEventRequest Request;
	Request.UUID = CrowdyWireParity::GoldenActorId();
	Request.AppID = 13;
	Request.ChunkX = -8;
	Request.ChunkY = 9;
	Request.ChunkZ = 10;
	Request.EventType = static_cast<uint16>(0xBEEF);
	Request.StateBytes = State;
	Request.StateSize = State.Num();
	Request.Target = ECrowdyTarget::Everyone;
	Request.TargetID = FGuid(1, 2, 3, 4);

	const TArray<uint8> Serialized = Request.Serialize();
	if (!TestTrue(TEXT("serialized event request carries a payload"),
		Serialized.Num() > static_cast<int32>(kLongSpatialHeaderSize + kEventTypeSize)))
	{
		return false;
	}

	const TArray<uint8> UnrealPayload(Serialized.GetData() + kLongSpatialHeaderSize,
		Serialized.Num() - static_cast<int32>(kLongSpatialHeaderSize));

	// Everything after the event type is opaque to the shared codec, so feeding it that exact region
	// is what parity means here: the event type framing and the absence of any extra framing.
	const TArray<uint8> OpaqueState(UnrealPayload.GetData() + kEventTypeSize,
		UnrealPayload.Num() - static_cast<int32>(kEventTypeSize));

	TArray<uint8> Buffer;
	Buffer.SetNumZeroed(UnrealPayload.Num() + 64);
	const crowdy::Result<std::size_t> Written = encodeEventPayload(static_cast<std::uint16_t>(0xBEEF),
		crowdy::Bytes(OpaqueState.GetData(), static_cast<std::size_t>(OpaqueState.Num())),
		crowdy::MutableBytes(Buffer.GetData(), static_cast<std::size_t>(Buffer.Num())));

	if (!TestTrue(TEXT("shared event encoder succeeded"), Written.ok()))
	{
		return false;
	}

	const TArray<uint8> SharedPayload(Buffer.GetData(), static_cast<int32>(Written.value()));

	TestTrue(FString::Printf(TEXT("event payload encodes identically (%s)"),
		*CrowdyWireParity::DescribeDifference(UnrealPayload, SharedPayload)),
		UnrealPayload == SharedPayload);

	// What the engine puts in that opaque region is not just the state: it writes a four-byte state
	// length, then the state, then a one-byte routing target and a 32-byte target id. A reader that
	// takes the shared codec's state view as the event state alone will be wrong by those 37 bytes
	// plus the length prefix, so anything decoding these events has to parse that structure itself.
	TArray<uint8> StateOnlyBuffer;
	StateOnlyBuffer.SetNumZeroed(State.Num() + 64);
	const crowdy::Result<std::size_t> StateOnlyWritten = encodeEventPayload(static_cast<std::uint16_t>(0xBEEF),
		crowdy::Bytes(State.GetData(), static_cast<std::size_t>(State.Num())),
		crowdy::MutableBytes(StateOnlyBuffer.GetData(), static_cast<std::size_t>(StateOnlyBuffer.Num())));

	if (!TestTrue(TEXT("shared event encoder succeeded for the state alone"), StateOnlyWritten.ok()))
	{
		return false;
	}

	const TArray<uint8> StateOnlyPayload(StateOnlyBuffer.GetData(), static_cast<int32>(StateOnlyWritten.value()));

	// Comparing the two buffers outright only ever reports that they differ, since one is shorter by the
	// length prefix and the whole envelope. What a decoder actually needs is where the divergence starts:
	// both forms open with the same event type, and the engine's first field after it is the state length.
	TestEqual(TEXT("the state-only payload is the event type and the state and nothing else"),
		StateOnlyPayload.Num(), static_cast<int32>(kEventTypeSize) + State.Num());
	TestEqual(TEXT("the payload region carries a length prefix and a routing envelope"),
		UnrealPayload.Num(),
		static_cast<int32>(kEventTypeSize) + static_cast<int32>(sizeof(int32)) + State.Num() + 1 + 32);

	if (UnrealPayload.Num() >= static_cast<int32>(kEventTypeSize) + static_cast<int32>(sizeof(int32))
		&& StateOnlyPayload.Num() >= static_cast<int32>(kEventTypeSize))
	{
		TestTrue(TEXT("both forms open with the same event type"),
			FMemory::Memcmp(UnrealPayload.GetData(), StateOnlyPayload.GetData(), kEventTypeSize) == 0);
		TestEqual(TEXT("the engine writes the state length immediately after the event type"),
			static_cast<int32>(crowdy::le::readU32(UnrealPayload.GetData() + kEventTypeSize)),
			State.Num());
	}

	return true;
}

#endif
