#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Network/UDP/CrowdyCppSendAdapter.h"
#include "Network/UDP/CrowdyCppSendAdapterInternal.h"

#include "Serialization/CrowdyWireParitySupport.h"

namespace CrowdySendAdapterTestSupport
{
	constexpr EAutomationTestFlags SendAdapterTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** The fixed spatial header, restated here so a test fails if the adapter and the encoder ever disagree on it. */
	constexpr int32 SpatialHeaderSize = MetadataSize + 1;

	FActorUpdateRequestMessage MakeActorUpdate()
	{
		FActorUpdateRequestMessage Message;
		Message.AppID = 7;
		Message.ChunkX = 1;
		Message.ChunkY = -2;
		Message.ChunkZ = 3;
		Message.UUID = CrowdyWireParity::GoldenActorId();
		Message.ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks;
		Message.DecayRate = ECrowdyDecayRate::Exponential_Decay;
		Message.StateBytes = {0xaa, 0xbb};
		Message.StateSize = Message.StateBytes.Num();
		return Message;
	}

	FString HexOf(const TArrayView<const uint8> Bytes)
	{
		return CrowdyWireParity::ToHex(TArray<uint8>(Bytes.GetData(), Bytes.Num()));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySendAdapterSpatialSplitTest,
	"CrowdySDK.Transport.SendAdapterSplitsASpatialMessage",
	CrowdySendAdapterTestSupport::SendAdapterTestFlags)

bool FCrowdySendAdapterSpatialSplitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySendAdapterTestSupport;

	const FActorUpdateRequestMessage Message = MakeActorUpdate();
	const TArray<uint8> Serialized = Message.Serialize();

	// The storage outlives the frame on purpose: the frame's actor id and payload are views into it.
	TArray<uint8> Storage;
	FCrowdyCppOutboundFrame Frame;
	FString Error;

	// Split first, then assert. Reading Error inside an argument to the same call that fills it leaves the two
	// unsequenced, so the reason could come out empty in exactly the run where it is needed.
	const bool bSplit = CrowdyCppSend::SplitMessage(Message, Storage, Frame, Error);
	if (!TestTrue(FString::Printf(TEXT("the message split (%s)"), *Error), bSplit))
	{
		return false;
	}

	TestFalse(TEXT("an actor update is not a channel message"), Frame.bIsChannel);
	TestEqual(TEXT("the opcode is the actor update request"), static_cast<int32>(Frame.Opcode),
		static_cast<int32>(ECrowdyMessageType::ACTOR_UPDATE_REQUEST));
	TestEqual(TEXT("the app id came back"), Frame.AppId, static_cast<int64>(7));
	TestEqual(TEXT("chunk x came back"), Frame.ChunkX, static_cast<int64>(1));

	// Signed and negative: reading a chunk coordinate as unsigned would pass every other assertion here.
	TestEqual(TEXT("a negative chunk y came back with its sign"), Frame.ChunkY, static_cast<int64>(-2));
	TestEqual(TEXT("chunk z came back"), Frame.ChunkZ, static_cast<int64>(3));
	TestEqual(TEXT("the replication distance came back"), static_cast<int32>(Frame.Distance), 8);
	TestEqual(TEXT("the decay rate came back"), static_cast<int32>(Frame.Decay),
		static_cast<int32>(ECrowdyDecayRate::Exponential_Decay));

	TestEqual(TEXT("the actor id is 32 octets"), Frame.Uuid.Num(), 32);
	TArray<uint8> ExpectedUuid;
	ExpectedUuid.Append(reinterpret_cast<const uint8*>(CrowdyWireParity::GoldenUuid()), 32);
	TestEqual(TEXT("the actor id is the one the message carried"), HexOf(Frame.Uuid), HexOf(ExpectedUuid));

	// The payload is the region past the fixed header and nothing else: the length field the message writes is part
	// of the payload, not part of the header.
	TArray<uint8> ExpectedPayload;
	ExpectedPayload.Append({0x02, 0x00, 0x00, 0x00});
	ExpectedPayload.Append({0xaa, 0xbb});
	TestEqual(TEXT("the payload is the declared length followed by the state"), HexOf(Frame.Payload),
		HexOf(ExpectedPayload));
	TestEqual(TEXT("the payload is exactly what follows the header"), Frame.Payload.Num(),
		Serialized.Num() - SpatialHeaderSize);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySendAdapterChannelSplitTest,
	"CrowdySDK.Transport.SendAdapterSplitsAChannelMessage",
	CrowdySendAdapterTestSupport::SendAdapterTestFlags)

bool FCrowdySendAdapterChannelSplitTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySendAdapterTestSupport;

	FChannelMessageRequest Message;
	Message.ChannelId = 4242;
	Message.UUID = CrowdyWireParity::GoldenActorId();
	Message.Payload = {0x01, 0x02, 0x03};

	TArray<uint8> Storage;
	FCrowdyCppOutboundFrame Frame;
	FString Error;
	const bool bSplit = CrowdyCppSend::SplitMessage(Message, Storage, Frame, Error);
	if (!TestTrue(FString::Printf(TEXT("the channel message split (%s)"), *Error), bSplit))
	{
		return false;
	}

	TestTrue(TEXT("a channel publish is recognised as one"), Frame.bIsChannel);
	TestEqual(TEXT("the channel id came back"), Frame.ChannelId, static_cast<int64>(4242));
	TestEqual(TEXT("the payload is the published bytes alone"), HexOf(Frame.Payload),
		HexOf(TArray<uint8>({0x01, 0x02, 0x03})));
	TestEqual(TEXT("the sender id is 32 octets"), Frame.Uuid.Num(), 32);

	// A channel message reaches every member wherever they are, so there is nothing for the server to fan out by and
	// these must not carry a stale spatial value.
	TestEqual(TEXT("a channel message carries no chunk"), Frame.ChunkX + Frame.ChunkY + Frame.ChunkZ,
		static_cast<int64>(0));
	TestEqual(TEXT("a channel message carries no distance"), static_cast<int32>(Frame.Distance), 0);
	TestEqual(TEXT("a channel message carries no decay"), static_cast<int32>(Frame.Decay), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySendAdapterRefusalsTest,
	"CrowdySDK.Transport.SendAdapterRefusesFramesItCannotRoute",
	CrowdySendAdapterTestSupport::SendAdapterTestFlags)

bool FCrowdySendAdapterRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace CrowdySendAdapterTestSupport;

	FString Error;

	// A message whose actor id was never filled in addresses nobody. The header reserves a fixed 32 octets for it,
	// so the bytes would look well formed while naming no actor, which is why this is caught against the object.
	{
		FActorUpdateRequestMessage Message = MakeActorUpdate();
		Message.UUID.Reset();

		// Pre-loaded with something, so a refusal that left the frame alone would be visible: the views would still
		// be pointing at bytes describing a different message.
		const TArray<uint8> Leftover = {0xff};
		TArray<uint8> Storage;
		FCrowdyCppOutboundFrame Frame;
		Frame.Payload = Leftover;
		TestFalse(TEXT("a message with no actor id is refused"),
			CrowdyCppSend::SplitMessage(Message, Storage, Frame, Error));
		TestTrue(TEXT("the refusal says what was wrong"), Error.Contains(TEXT("octets")));
		TestEqual(TEXT("a refused split leaves nothing behind in the frame"), Frame.Payload.Num(), 0);
		TestEqual(TEXT("and nothing behind in its storage"), Storage.Num(), 0);
	}

	// The connection signs every frame it sends, so a message that asked to go unsigned would go out as a different
	// frame from the one the caller built.
	{
		FActorUpdateRequestMessage Message = MakeActorUpdate();
		Message.bContainsAuth = false;

		TArray<uint8> Storage;
		FCrowdyCppOutboundFrame Frame;
		TestFalse(TEXT("an unsigned spatial message is refused"),
			CrowdyCppSend::SplitMessage(Message, Storage, Frame, Error));
		TestTrue(TEXT("the refusal names the signing"), Error.Contains(TEXT("signed")));
	}

	// Nothing in the codebase builds this today. It is guarded because the length field and the payload are written
	// from the same array by one encoder, so a disagreement means the frame did not come from that encoder.
	{
		TArray<uint8> Forged;
		Forged.Add(static_cast<uint8>(ECrowdyMessageType::CHANNEL_MESSAGE_REQUEST));
		Forged.Append({0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
		Forged.Append(reinterpret_cast<const uint8*>(CrowdyWireParity::GoldenUuid()), 32);
		Forged.Append({0x10, 0x00});                // declares 16 payload octets
		Forged.Append({0xaa, 0xbb});                // and carries 2
		Forged.Add(1);

		FCrowdyCppOutboundFrame Frame;
		TestFalse(TEXT("a channel frame that lies about its length is refused"),
			CrowdyCppSend::SplitSerializedFrame(Forged, Frame, Error));
		TestTrue(TEXT("the refusal reports both lengths"), Error.Contains(TEXT("declares")));
	}

	// A truncated frame must be refused rather than read past.
	{
		TArray<uint8> Short;
		Short.Add(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_REQUEST));
		Short.Append({0x00, 0x00, 0x00});

		FCrowdyCppOutboundFrame Frame;
		TestFalse(TEXT("a frame shorter than the header is refused"),
			CrowdyCppSend::SplitSerializedFrame(Short, Frame, Error));
	}

	{
		FCrowdyCppOutboundFrame Frame;
		TestFalse(TEXT("an empty frame is refused"),
			CrowdyCppSend::SplitSerializedFrame(TArrayView<const uint8>(), Frame, Error));
	}

	return true;
}

#endif
