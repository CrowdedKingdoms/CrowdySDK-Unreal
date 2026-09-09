#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CrowdyWireParitySupport.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyWireLayoutTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// The Unreal datagram layout and the shared C++ SDK's layout table are two independent transcriptions
// of one wire format. Nothing forces them to agree, so a change to either side stays silent until a
// live server rejects a datagram. These assertions turn that divergence into a test failure.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireLayoutConstantsTest,
	"CrowdySDK.Wire.LayoutConstants", CrowdyWireLayoutTestFlags)
bool FCrowdyWireLayoutConstantsTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	TestEqual(TEXT("spatial header size including the type byte"),
		static_cast<uint32>(MetadataSize + 1), static_cast<uint32>(kLongSpatialHeaderSize));
	TestEqual(TEXT("unsigned trailer size"),
		static_cast<uint32>(TailSize), static_cast<uint32>(kTailNoHmac));
	TestEqual(TEXT("signed trailer size"),
		static_cast<uint32>(TailSize + kHmacTagSize), static_cast<uint32>(kTailWithHmac));

	TestEqual(TEXT("type offset"), static_cast<uint32>(offsets::kType), 0u);
	TestEqual(TEXT("app id offset"), static_cast<uint32>(offsets::kAppId), 1u);
	TestEqual(TEXT("chunk x offset"), static_cast<uint32>(offsets::kChunkX), 9u);
	TestEqual(TEXT("chunk y offset"), static_cast<uint32>(offsets::kChunkY), 17u);
	TestEqual(TEXT("chunk z offset"), static_cast<uint32>(offsets::kChunkZ), 25u);
	TestEqual(TEXT("distance offset"), static_cast<uint32>(offsets::kDistance), 33u);
	TestEqual(TEXT("decay offset"), static_cast<uint32>(offsets::kDecay), 34u);
	TestEqual(TEXT("contains auth offset"), static_cast<uint32>(offsets::kContainsAuth), 35u);
	TestEqual(TEXT("uuid offset"), static_cast<uint32>(offsets::kUuid), 36u);
	TestEqual(TEXT("payload offset"), static_cast<uint32>(offsets::kPayload), 68u);

	TestEqual(TEXT("hmac tag size"), static_cast<uint32>(kHmacTagSize), 32u);
	TestEqual(TEXT("token octets"), static_cast<uint32>(kTokenOctets), 64u);
	TestEqual(TEXT("uuid size"), static_cast<uint32>(kUuidSize), 32u);

	return true;
}

// Opcode values are the one part of the format that a compiler cannot check across implementations,
// and a renumbering on either side would silently reroute traffic rather than fail to build.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireOpcodeAgreementTest,
	"CrowdySDK.Wire.OpcodeAgreement", CrowdyWireLayoutTestFlags)
bool FCrowdyWireOpcodeAgreementTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const auto CheckOpcode = [this](const TCHAR* Name, const ECrowdyMessageType Unreal, const MessageType Shared)
	{
		TestEqual(Name, static_cast<uint32>(Unreal), static_cast<uint32>(Shared));
	};

	CheckOpcode(TEXT("MESSAGE_BUNDLE"), ECrowdyMessageType::MESSAGE_BUNDLE, MessageType::MessageBundle);
	CheckOpcode(TEXT("GENERIC_ERROR_MESSAGE"), ECrowdyMessageType::GENERIC_ERROR_MESSAGE, MessageType::GenericError);
	CheckOpcode(TEXT("CHANNEL_MESSAGE_REQUEST"), ECrowdyMessageType::CHANNEL_MESSAGE_REQUEST, MessageType::ChannelMessageRequest);
	CheckOpcode(TEXT("CHANNEL_MESSAGE_NOTIFICATION"), ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION, MessageType::ChannelMessageNotification);
	CheckOpcode(TEXT("ACTOR_UPDATE_REQUEST"), ECrowdyMessageType::ACTOR_UPDATE_REQUEST, MessageType::ActorUpdateRequest);
	CheckOpcode(TEXT("ACTOR_UPDATE_NOTIFICATION"), ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION, MessageType::ActorUpdateNotification);
	CheckOpcode(TEXT("VOXEL_UPDATE_REQUEST"), ECrowdyMessageType::VOXEL_UPDATE_REQUEST, MessageType::VoxelUpdateRequest);
	CheckOpcode(TEXT("VOXEL_UPDATE_NOTIFICATION"), ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION, MessageType::VoxelUpdateNotification);
	CheckOpcode(TEXT("CLIENT_AUDIO_PACKET"), ECrowdyMessageType::CLIENT_AUDIO_PACKET, MessageType::ClientAudioPacket);
	CheckOpcode(TEXT("CLIENT_AUDIO_NOTIFICATION"), ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION, MessageType::ClientAudioNotification);
	CheckOpcode(TEXT("CLIENT_VIDEO_PACKET"), ECrowdyMessageType::CLIENT_VIDEO_PACKET, MessageType::ClientVideoPacket);
	CheckOpcode(TEXT("CLIENT_VIDEO_NOTIFICATION"), ECrowdyMessageType::CLIENT_VIDEO_NOTIFICATION,
		MessageType::ClientVideoNotification);
	CheckOpcode(TEXT("CLIENT_TEXT_PACKET"), ECrowdyMessageType::CLIENT_TEXT_PACKET, MessageType::ClientTextPacket);
	CheckOpcode(TEXT("CLIENT_TEXT_NOTIFICATION"), ECrowdyMessageType::CLIENT_TEXT_NOTIFICATION, MessageType::ClientTextNotification);
	CheckOpcode(TEXT("CLIENT_EVENT_NOTIFICATION"), ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION, MessageType::ClientEventNotification);
	CheckOpcode(TEXT("SERVER_EVENT_NOTIFICATION"), ECrowdyMessageType::SERVER_EVENT_NOTIFICATION, MessageType::ServerEventNotification);
	CheckOpcode(TEXT("GENERIC_SPATIAL_1"), ECrowdyMessageType::GENERIC_SPATIAL_1, MessageType::GenericSpatial1);
	CheckOpcode(TEXT("SINGLE_ACTOR_MESSAGE"), ECrowdyMessageType::SINGLE_ACTOR_MESSAGE, MessageType::SingleActorMessage);
	CheckOpcode(TEXT("ACTOR_LEFT_NOTIFICATION"), ECrowdyMessageType::ACTOR_LEFT_NOTIFICATION,
		MessageType::ActorLeftNotification);

	// Every opcode Unreal sends through the shared spatial header must be one the shared codec also
	// treats as long-spatial, or the two disagree about where the payload starts.
	TestTrue(TEXT("128 uses the long spatial layout"), isLongSpatialLayout(128));
	TestTrue(TEXT("138 uses the long spatial layout"), isLongSpatialLayout(138));
	TestTrue(TEXT("139 uses the long spatial layout"), isLongSpatialLayout(139));
	TestTrue(TEXT("140 uses the long spatial layout"), isLongSpatialLayout(140));
	TestTrue(TEXT("142 uses the long spatial layout"), isLongSpatialLayout(142));
	TestTrue(TEXT("143 uses the long spatial layout"), isLongSpatialLayout(143));
	TestTrue(TEXT("144 uses the long spatial layout"), isLongSpatialLayout(144));
	TestTrue(TEXT("145 uses the long spatial layout"), isLongSpatialLayout(145));
	TestFalse(TEXT("141 is reserved and unimplemented"), isLongSpatialLayout(141));

	return true;
}

#endif
