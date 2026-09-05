#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "CrowdyWireParitySupport.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyWireGoldenTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
}

// Comparing the two implementations against each other proves they agree, not that either is right;
// they can drift together. This vector was derived from the published wire-format and HMAC
// documentation alone, so it is the one check that can catch a shared mistake.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireGoldenSpatialEncodeTest,
	"CrowdySDK.Wire.GoldenSpatialEncode", CrowdyWireGoldenTestFlags)
bool FCrowdyWireGoldenSpatialEncodeTest::RunTest(const FString& Parameters)
{
	CrowdyWireParity::FParitySpatialMessage Message;
	Message.TypeOverride = ECrowdyMessageType::GENERIC_SPATIAL_1;
	Message.AppID = 7;
	Message.ChunkX = 1;
	Message.ChunkY = -2;
	Message.ChunkZ = 3;
	Message.ReplicationDistance = static_cast<ECrowdyReplicationDistance>(8);
	Message.DecayRate = static_cast<ECrowdyDecayRate>(1);
	Message.bContainsAuth = true;
	Message.UUID = CrowdyWireParity::GoldenActorId();
	Message.PayloadBytes = { 0xde, 0xad, 0xbe, 0xef };

	const TArray<uint8> Datagram = CrowdyWireParity::AppendSignedTail(
		Message.Serialize(), CrowdyWireParity::GoldenToken(), 123456789, 42, true);

	const TArray<uint8> Golden = CrowdyWireParity::GoldenSpatialDatagram();

	TestEqual(TEXT("datagram length"), Datagram.Num(), Golden.Num());
	TestTrue(FString::Printf(TEXT("Unreal encoder matches the golden datagram (%s)"),
		*CrowdyWireParity::DescribeDifference(Datagram, Golden)), Datagram == Golden);

	return true;
}

// The same vector, against the shared C++ SDK's encoder. Running both against one third-party anchor
// is what makes the cross-implementation comparisons downstream meaningful.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireGoldenSharedEncodeTest,
	"CrowdySDK.Wire.GoldenSharedEncode", CrowdyWireGoldenTestFlags)
bool FCrowdyWireGoldenSharedEncodeTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const uint8 Payload[] = { 0xde, 0xad, 0xbe, 0xef };

	LongSpatialParams Params{};
	Params.type = MessageType::GenericSpatial1;
	Params.appId = 7;
	Params.chunk = ChunkCoord{ 1, -2, 3 };
	Params.distance = 8;
	Params.decay = DecayRate::Exponential;
	Params.payload = crowdy::Bytes(Payload, UE_ARRAY_COUNT(Payload));
	Params.gameTokenId = 123456789;
	Params.sequence = 42;
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
	const TArray<uint8> Golden = CrowdyWireParity::GoldenSpatialDatagram();

	TestTrue(FString::Printf(TEXT("shared encoder matches the golden datagram (%s)"),
		*CrowdyWireParity::DescribeDifference(Datagram, Golden)), Datagram == Golden);

	return true;
}

// Verification is the receive-side half of the same rule, and a tag that validates a tampered payload
// is worth more to catch than one that rejects a good frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireGoldenVerifyTest,
	"CrowdySDK.Wire.GoldenVerify", CrowdyWireGoldenTestFlags)
bool FCrowdyWireGoldenVerifyTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const TArray<uint8> Golden = CrowdyWireParity::GoldenSpatialDatagram();
	const FString Token = CrowdyWireParity::GoldenToken();

	TestTrue(TEXT("Unreal authenticates the golden datagram"),
		USerializationFunctionLibrary::AuthenticateHMAC(Golden, Token));

	const crowdy::Status SharedVerdict = verifyLongSpatial(CrowdyWireParity::Crypto(),
		crowdy::Bytes(Golden.GetData(), Golden.Num()), CrowdyWireParity::ParityToken());
	TestTrue(TEXT("shared codec authenticates the golden datagram"), SharedVerdict.ok());

	// A single flipped payload bit must fail on both sides.
	TArray<uint8> Tampered = Golden;
	Tampered[offsets::kPayload] ^= 0x01;

	TestFalse(TEXT("Unreal rejects a tampered payload"),
		USerializationFunctionLibrary::AuthenticateHMAC(Tampered, Token));

	const crowdy::Status TamperedVerdict = verifyLongSpatial(CrowdyWireParity::Crypto(),
		crowdy::Bytes(Tampered.GetData(), Tampered.Num()), CrowdyWireParity::ParityToken());
	TestFalse(TEXT("shared codec rejects a tampered payload"), TamperedVerdict.ok());

	// A correctly-formed datagram signed under a different token must also fail.
	FString WrongToken = CrowdyWireParity::GoldenToken();
	WrongToken[0] = TEXT('z');
	TestFalse(TEXT("Unreal rejects the wrong token"),
		USerializationFunctionLibrary::AuthenticateHMAC(Golden, WrongToken));

	return true;
}

// Unreal has no encoder or verifier for COMMAND_RECONNECT, so this pins the frame the shared codec
// accepts. It documents what the transport swap has to keep accepting.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireGoldenReconnectTest,
	"CrowdySDK.Wire.GoldenReconnect", CrowdyWireGoldenTestFlags)
bool FCrowdyWireGoldenReconnectTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const TArray<uint8> Frame = CrowdyWireParity::GoldenReconnectFrame();
	TestEqual(TEXT("reconnect frame size"), Frame.Num(), static_cast<int32>(kCommandReconnectSize));

	const crowdy::Status Verdict = verifyCommandReconnect(CrowdyWireParity::Crypto(),
		crowdy::Bytes(Frame.GetData(), Frame.Num()), CrowdyWireParity::ParityToken());
	TestTrue(TEXT("shared codec verifies the golden reconnect frame"), Verdict.ok());

	TArray<uint8> Forged = Frame;
	Forged[1] ^= 0x01;
	const crowdy::Status ForgedVerdict = verifyCommandReconnect(CrowdyWireParity::Crypto(),
		crowdy::Bytes(Forged.GetData(), Forged.Num()), CrowdyWireParity::ParityToken());
	TestFalse(TEXT("shared codec rejects a forged reconnect frame"), ForgedVerdict.ok());

	return true;
}

// The exported crypto provider is a plain HMAC primitive check, independent of datagram layout:
// it signs the same bytes CalculateHMAC signs and must land on the same tag. A provider that
// fails every primitive would otherwise make the rest of this suite fail in a confusing way
// rather than naming the cause, so availability is checked first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyWireGoldenPromotedCryptoTagTest,
	"CrowdySDK.Wire.GoldenPromotedCryptoTag", CrowdyWireGoldenTestFlags)
bool FCrowdyWireGoldenPromotedCryptoTagTest::RunTest(const FString& Parameters)
{
	using namespace crowdy::wire;

	const crowdy::core::ICrypto& Crypto = GetCrowdyCppCrypto();
	if (!TestTrue(TEXT("exported crypto provider is available"), Crypto.availability().ok()))
	{
		return false;
	}

	const TArray<uint8> Golden = CrowdyWireParity::GoldenSpatialDatagram();
	const ANSICHAR* Token = CrowdyWireParity::GoldenToken();
	check(FCStringAnsi::Strlen(Token) == kTokenOctets);

	TArray<uint8> Key;
	Key.Append(reinterpret_cast<const uint8*>(Token), static_cast<int32>(kTokenOctets));

	const std::size_t PrefixLen = offsets::kPayload + 4; // header + the 4-byte payload in this vector
	TArray<uint8> Message;
	Message.Append(Golden.GetData(), static_cast<int32>(PrefixLen));
	Message.Append(Key);

	uint8 Tag[kHmacTagSize] = {};
	const bool bSigned = Crypto.hmacSha256(
		crowdy::Bytes(Key.GetData(), Key.Num()),
		crowdy::Bytes(Message.GetData(), Message.Num()),
		Tag);
	TestTrue(TEXT("exported crypto provider signs the message"), bSigned);

	TArray<uint8> Actual(Tag, kHmacTagSize);
	TArray<uint8> Expected(Golden.GetData() + PrefixLen, kHmacTagSize);

	TestTrue(FString::Printf(TEXT("exported crypto provider matches the golden HMAC tag (%s)"),
		*CrowdyWireParity::DescribeDifference(Actual, Expected)), Actual == Expected);

	return true;
}

#endif
