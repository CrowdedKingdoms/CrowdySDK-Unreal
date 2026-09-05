#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Serialization/CrowdyFrameSplit.h"
#include "Utils/SerializationFunctionLibrary.h"

// Where the envelope comes off a frame received as bytes. This is the half of the seam a transport that
// receives datagrams performs and a transport that is handed decoded messages does not, so a divergence
// here is a divergence between the two transports.
namespace
{
	constexpr EAutomationTestFlags CrowdyFrameSplitTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const ANSICHAR* SplitTestUuid() { return "0123456789abcdef0123456789abcdef"; }

	template <typename T>
	void AppendValue(TArray<uint8>& Out, T Value)
	{
		Out.Append(USerializationFunctionLibrary::SerializeValue<T>(Value));
	}

	/** The spatial header, exactly as a server writes it, with the opcode already taken off. */
	TArray<uint8> BuildSpatialHeader(const bool bSigned)
	{
		TArray<uint8> Out;
		AppendValue<int64>(Out, 4242);
		AppendValue<int64>(Out, 1);
		AppendValue<int64>(Out, -2);
		AppendValue<int64>(Out, 3);
		Out.Add(8);                                     // fan-out distance
		Out.Add(1);                                     // decay rate
		Out.Add(bSigned ? 1 : 0);                       // signature flag
		Out.Append(reinterpret_cast<const uint8*>(SplitTestUuid()), 32);
		check(Out.Num() == MetadataSize);
		return Out;
	}

	void AppendTrailer(TArray<uint8>& Out, const int64 Timestamp, const uint8 Sequence)
	{
		AppendValue<int64>(Out, Timestamp);
		Out.Add(Sequence);
	}

	/** 32 octets standing in for a server signature. Only their count matters to the split. */
	void AppendSignature(TArray<uint8>& Out)
	{
		for (int32 Index = 0; Index < 32; ++Index)
		{
			Out.Add(static_cast<uint8>(0xF0 + (Index & 0x0F)));
		}
	}

	FString DescribeBody(const FCrowdyFrame& Frame)
	{
		return FString::Printf(TEXT("%d:%s"), Frame.Body.Num(),
			*BytesToHex(Frame.Body.GetData(), Frame.Body.Num()));
	}
}

// A signed frame carries 32 signature octets between its body and its trailer. Nothing else in the suite
// covered that: every hand-built vector elsewhere describes an unsigned frame, so before this the signed
// layout was exercised only by the live-socket parity test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFrameSplitSignedBodyTest,
	"CrowdySDK.Wire.FrameSplitSignedFrameExcludesTheSignature", CrowdyFrameSplitTestFlags)
bool FCrowdyFrameSplitSignedBodyTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Body = { 0xA1, 0xA2, 0xA3, 0xA4 };

	TArray<uint8> Signed = BuildSpatialHeader(true);
	Signed.Append(Body);
	AppendSignature(Signed);
	AppendTrailer(Signed, 1700000000000LL, 77);

	FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION), Signed);
	if (!TestTrue(TEXT("a signed frame splits"), CrowdyFrameSplit::ReadSpatialEnvelope(Frame)))
	{
		return false;
	}

	TestEqual(TEXT("the body is the payload alone, with no signature in it"),
		DescribeBody(Frame), FString::Printf(TEXT("4:%s"), *BytesToHex(Body.GetData(), Body.Num())));
	TestTrue(TEXT("the frame reports that it was signed"), Frame.Envelope.bContainsAuth);

	// Read from the very end, so a body length taken as the difference from the end of the buffer would
	// have to be right for these two to be.
	TestEqual(TEXT("the timestamp comes off the end"), Frame.Envelope.Timestamp, 1700000000000LL);
	TestEqual(TEXT("the sequence is the last octet"), static_cast<int32>(Frame.Envelope.Sequence), 77);

	TestEqual(TEXT("the actor id is the 32 octets of the header"), Frame.Envelope.Uuid.Num(), 32);
	TestEqual(TEXT("and it is the id the header carried"),
		BytesToHex(Frame.Envelope.Uuid.GetData(), 32),
		BytesToHex(reinterpret_cast<const uint8*>(SplitTestUuid()), 32));

	// The same bytes without the signature and with the flag clear: the body has to come out the same,
	// which is what says the flag and not the length is what decides where the body ends.
	TArray<uint8> Unsigned = BuildSpatialHeader(false);
	Unsigned.Append(Body);
	AppendTrailer(Unsigned, 1700000000000LL, 77);

	FCrowdyFrame UnsignedFrame(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION), Unsigned);
	if (TestTrue(TEXT("an unsigned frame splits"), CrowdyFrameSplit::ReadSpatialEnvelope(UnsignedFrame)))
	{
		TestEqual(TEXT("and yields the same body"), DescribeBody(UnsignedFrame), DescribeBody(Frame));
		TestFalse(TEXT("while reporting no signature"), UnsignedFrame.Envelope.bContainsAuth);
	}

	return true;
}

// A frame whose header claims a signature it does not carry cannot be read: the trailer it declares runs
// past its own start. Refusing it is what keeps a decoder from taking 32 octets of somebody else's data
// as its body, or from computing a negative length.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFrameSplitBoundariesTest,
	"CrowdySDK.Wire.FrameSplitRefusesWhatCannotHoldItsOwnEnvelope", CrowdyFrameSplitTestFlags)
bool FCrowdyFrameSplitBoundariesTest::RunTest(const FString& Parameters)
{
	// Signed, but with only the trailer behind the header: 32 octets short of what the flag promises.
	{
		TArray<uint8> Data = BuildSpatialHeader(true);
		AppendTrailer(Data, 1, 1);

		FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION), Data);
		TestFalse(TEXT("a frame claiming a signature it does not carry is refused"),
			CrowdyFrameSplit::ReadSpatialEnvelope(Frame));
	}

	// Unsigned, with exactly the header and the trailer: the shortest frame that can be read at all, and
	// its body is empty rather than absent.
	{
		TArray<uint8> Data = BuildSpatialHeader(false);
		AppendTrailer(Data, 55, 6);

		FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION), Data);
		if (TestTrue(TEXT("the shortest readable frame splits"), CrowdyFrameSplit::ReadSpatialEnvelope(Frame)))
		{
			TestEqual(TEXT("with an empty body"), Frame.Body.Num(), 0);
			TestEqual(TEXT("and its trailer read"), Frame.Envelope.Timestamp, static_cast<int64>(55));
		}
	}

	// One octet shorter than that.
	{
		TArray<uint8> Data = BuildSpatialHeader(false);
		AppendTrailer(Data, 55, 6);
		Data.SetNum(Data.Num() - 1);

		FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION), Data);
		TestFalse(TEXT("one octet short of the header and trailer is refused"),
			CrowdyFrameSplit::ReadSpatialEnvelope(Frame));
	}

	// A header cut short before the signature flag it has to read first.
	{
		TArray<uint8> Data = BuildSpatialHeader(false);
		Data.SetNum(20);

		FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION), Data);
		TestFalse(TEXT("a frame too short to hold its header is refused"),
			CrowdyFrameSplit::ReadSpatialEnvelope(Frame));
	}

	return true;
}

// The channel layout declares its own payload length, so the body is what the header says it is rather
// than what is left over, and its trailer is optional.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFrameSplitChannelTest,
	"CrowdySDK.Wire.FrameSplitChannelBodyIsTheDeclaredLength", CrowdyFrameSplitTestFlags)
bool FCrowdyFrameSplitChannelTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Payload = { 0xDE, 0xAD, 0xBE, 0xEF };

	TArray<uint8> Data;
	AppendValue<int64>(Data, 9001);
	Data.Append(reinterpret_cast<const uint8*>(SplitTestUuid()), 32);
	AppendValue<uint16>(Data, static_cast<uint16>(Payload.Num()));
	Data.Append(Payload);
	AppendTrailer(Data, 1700000000002LL, 21);

	{
		FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION), Data);
		if (TestTrue(TEXT("a channel frame splits"), CrowdyFrameSplit::ReadChannelEnvelope(Frame)))
		{
			TestEqual(TEXT("the channel id"), Frame.Envelope.ChannelId, static_cast<int64>(9001));
			TestEqual(TEXT("the body is the declared payload"), DescribeBody(Frame),
				FString::Printf(TEXT("4:%s"), *BytesToHex(Payload.GetData(), Payload.Num())));
			TestEqual(TEXT("the timestamp"), Frame.Envelope.Timestamp, 1700000000002LL);
			TestEqual(TEXT("the sequence"), static_cast<int32>(Frame.Envelope.Sequence), 21);
		}
	}

	// The trailer is part of the layout, and the vendored decoder refuses a notification without one, so
	// this reader has to as well: a frame only one of the two transports accepts is a divergence between
	// them, which is the thing the frame seam exists to remove.
	{
		TArray<uint8> NoTrailer(Data.GetData(), Data.Num() - 9);
		FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION), NoTrailer);
		TestFalse(TEXT("a channel frame with no trailer is refused"),
			CrowdyFrameSplit::ReadChannelEnvelope(Frame));
	}

	// One octet short of the trailer, which is the boundary that refusal sits on.
	{
		TArray<uint8> ShortTrailer(Data.GetData(), Data.Num() - 1);
		FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION), ShortTrailer);
		TestFalse(TEXT("a channel frame one octet short of its trailer is refused"),
			CrowdyFrameSplit::ReadChannelEnvelope(Frame));
	}

	// A declared length that runs past the end.
	{
		TArray<uint8> Short(Data.GetData(), 45);
		FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION), Short);
		TestFalse(TEXT("a declared payload length running past the end is refused"),
			CrowdyFrameSplit::ReadChannelEnvelope(Frame));
	}

	return true;
}

// Which envelope an opcode carries is decided in one place, and both the parser and the fixtures read it
// from there, so an opcode added to one and not the other cannot happen.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyFrameSplitOpcodeMappingTest,
	"CrowdySDK.Wire.FrameSplitMapsOpcodesToEnvelopes", CrowdyFrameSplitTestFlags)
bool FCrowdyFrameSplitOpcodeMappingTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Spatial = BuildSpatialHeader(false);
	Spatial.Append({ 0x01, 0x02 });
	AppendTrailer(Spatial, 9, 3);

	const ECrowdyMessageType SpatialOpcodes[] = {
		ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION,
		ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION,
		ECrowdyMessageType::SERVER_EVENT_NOTIFICATION,
		ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION,
		ECrowdyMessageType::CLIENT_AUDIO_NOTIFICATION,
		ECrowdyMessageType::GENERIC_SPATIAL_1,
		ECrowdyMessageType::SINGLE_ACTOR_MESSAGE
	};

	for (const ECrowdyMessageType Opcode : SpatialOpcodes)
	{
		FCrowdyFrame Frame(static_cast<uint8>(Opcode), Spatial);
		if (TestTrue(FString::Printf(TEXT("opcode %u splits"), static_cast<uint8>(Opcode)),
			CrowdyFrameSplit::ReadEnvelopeForOpcode(Frame)))
		{
			TestEqual(FString::Printf(TEXT("opcode %u takes the spatial header off"),
				static_cast<uint8>(Opcode)), Frame.Body.Num(), 2);
			TestTrue(FString::Printf(TEXT("opcode %u reports an envelope"), static_cast<uint8>(Opcode)),
				Frame.bHasEnvelope);
		}
	}

	// An opcode that carries no envelope keeps every octet it was given, and says it has none.
	{
		const TArray<uint8> Response = { 9, 3 };
		FCrowdyFrame Frame(static_cast<uint8>(ECrowdyMessageType::ACTOR_UPDATE_RESPONSE), Response);
		if (TestTrue(TEXT("a response opcode splits"), CrowdyFrameSplit::ReadEnvelopeForOpcode(Frame)))
		{
			TestEqual(TEXT("and keeps its whole body"), Frame.Body.Num(), 2);
			TestFalse(TEXT("and reports no envelope"), Frame.bHasEnvelope);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
