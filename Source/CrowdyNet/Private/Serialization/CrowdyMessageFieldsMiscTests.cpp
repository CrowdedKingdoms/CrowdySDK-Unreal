#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Messages/Communication/FClientAudioNotification.h"
#include "Messages/FDefaultMessage.h"
#include "Messages/FPingTestMessage.h"
#include "Messages/Voxel/FVoxelUpdateNotificationMessage.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "CrowdyWireParitySupport.h"

// Characterization coverage for FVoxelUpdateNotificationMessage,
// FClientAudioNotification, FChannelMessageNotification, FPingTestMessage and FDefaultMessage. Every
// vector pins what the CURRENT DecodePayload body does with a fixed set of bytes, not what it should do,
// so this file is the gate the frame-based decode conversion has to reproduce.
namespace
{
	constexpr EAutomationTestFlags CrowdyMessageFieldsMiscTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Several vectors below deliberately fall short of the minimum length a spatial header plus trailer
	// needs. That failure path runs through an ensureMsgf, which UE only reports once per
	// callsite for the life of the process (a later identical failure elsewhere in the automation run
	// stays silent), so it is not a stable target for AddExpectedError's occurrence count and error-level
	// output is suppressed for the tests that can reach it, exactly as the sibling
	// CrowdyWireDecodeParityTests.cpp already does for the same reason. Every assertion below still fails
	// the test normally; this only affects log output.
	class FCrowdyMessageFieldsMiscNoisyTest : public FAutomationTestBase
	{
	public:
		FCrowdyMessageFieldsMiscNoisyTest(const FString& InName, const bool bInComplexTask)
			: FAutomationTestBase(InName, bInComplexTask)
		{
		}

		virtual bool SuppressLogErrors() override { return true; }
	};

	// The 67-byte block every spatial message reads first: AppID, the three chunk coordinates,
	// ReplicationDistance, DecayRate, bContainsAuth, then the 32-octet actor id. Uuid32 must be exactly
	// 32 ASCII characters, matching the fixed-width region the decoder reads.
	TArray<uint8> BuildCrowdyMetadataHeaderBytes(const int64 AppID, const int64 ChunkX, const int64 ChunkY,
		const int64 ChunkZ, const ECrowdyReplicationDistance Distance, const ECrowdyDecayRate Decay,
		const bool bContainsAuth, const FString& Uuid32)
	{
		TArray<uint8> Data;
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(AppID));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(ChunkX));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(ChunkY));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(ChunkZ));
		Data.Add(static_cast<uint8>(Distance));
		Data.Add(static_cast<uint8>(Decay));
		Data.Add(bContainsAuth ? static_cast<uint8>(1) : static_cast<uint8>(0));

		const FTCHARToUTF8 ConvertedUuid(*Uuid32);
		check(ConvertedUuid.Length() == 32);
		Data.Append(reinterpret_cast<const uint8*>(ConvertedUuid.Get()), ConvertedUuid.Length());

		return Data;
	}

	// The trailer is always anchored to the very end of the buffer, so a caller building a realistic
	// frame has to append these 9 bytes last, after every other field.
	void AppendCrowdyTrailerBytes(TArray<uint8>& Data, const int64 Timestamp, const uint8 SequenceNumber)
	{
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(Timestamp));
		Data.Add(SequenceNumber);
	}
}

// FVoxelUpdateNotificationMessage: metadata, then Vx/Vy/Vz/VoxelType, then an optional
// [StateSize][State] block gated on how many bytes are left in the frame.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsVoxelNotificationTest,
	FCrowdyMessageFieldsMiscNoisyTest, "CrowdySDK.Wire.FieldsFVoxelUpdateNotificationMessage",
	CrowdyMessageFieldsMiscTestFlags)
bool FCrowdyMessageFieldsVoxelNotificationTest::RunTest(const FString& Parameters)
{
	const FString Uuid = TEXT("AAAA1111BBBB2222CCCC3333DDDD4444");
	const TArray<uint8> State = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };

	TArray<uint8> Full = BuildCrowdyMetadataHeaderBytes(555, 1, -2, 3,
		ECrowdyReplicationDistance::Eight_Chunks, ECrowdyDecayRate::Linear_50, false, Uuid);
	// Vx, Vy, Vz, VoxelType: four int16 fields read in that order right after the metadata header.
	Full.Append(USerializationFunctionLibrary::SerializeValue<int16>(10));
	Full.Append(USerializationFunctionLibrary::SerializeValue<int16>(-20));
	Full.Append(USerializationFunctionLibrary::SerializeValue<int16>(30));
	Full.Append(USerializationFunctionLibrary::SerializeValue<int16>(99));
	// StateSize, then the state bytes it declares.
	Full.Append(USerializationFunctionLibrary::SerializeValue<uint16>(static_cast<uint16>(State.Num())));
	Full.Append(State);
	AppendCrowdyTrailerBytes(Full, 123456789, 42);

	// Positive acceptance: every field the body sets, decoded from a well-formed 91-byte frame.
	{
		FVoxelUpdateNotificationMessage Message;
		if (!TestTrue(TEXT("a well-formed voxel notification decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Full)))
		{
			return false;
		}

		TestEqual(TEXT("AppID"), Message.AppID, static_cast<int64>(555));
		TestEqual(TEXT("ChunkX"), Message.ChunkX, static_cast<int64>(1));
		TestEqual(TEXT("ChunkY"), Message.ChunkY, static_cast<int64>(-2));
		TestEqual(TEXT("ChunkZ"), Message.ChunkZ, static_cast<int64>(3));
		TestEqual(TEXT("ReplicationDistance"), static_cast<int32>(Message.ReplicationDistance),
			static_cast<int32>(ECrowdyReplicationDistance::Eight_Chunks));
		TestEqual(TEXT("DecayRate"), static_cast<int32>(Message.DecayRate),
			static_cast<int32>(ECrowdyDecayRate::Linear_50));
		TestFalse(TEXT("bContainsAuth"), Message.bContainsAuth);
		TestEqual(TEXT("UUID"), Message.UUID, FCrowdyActorId::FromStringOrUnset(Uuid));
		TestEqual(TEXT("Timestamp"), Message.Timestamp, static_cast<int64>(123456789));
		TestEqual(TEXT("SequenceNumber"), static_cast<int32>(Message.SequenceNumber), 42);

		TestEqual(TEXT("Vx"), static_cast<int32>(Message.Vx), 10);
		TestEqual(TEXT("Vy"), static_cast<int32>(Message.Vy), -20);
		TestEqual(TEXT("Vz"), static_cast<int32>(Message.Vz), 30);
		TestEqual(TEXT("VoxelType"), static_cast<int32>(Message.VoxelType), 99);
		TestEqual(TEXT("StateSize"), static_cast<int32>(Message.StateSize), State.Num());
		TestTrue(TEXT("bContainsState"), Message.bContainsState);
		TestEqual(TEXT("StateBytes length"), Message.StateBytes.Num(), State.Num());
		if (Message.StateBytes.Num() == State.Num())
		{
			TestTrue(TEXT("StateBytes content"),
				FMemory::Memcmp(Message.StateBytes.GetData(), State.GetData(), State.Num()) == 0);
		}
	}

	// DataLength <= 0: rejected before any field is touched, no log at all.
	{
		FVoxelUpdateNotificationMessage Message;
		TestFalse(TEXT("an empty buffer is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, TArray<uint8>()));
	}

	// One byte short of the decoder's own minimum (MetadataSize + TailSize = 76).
	{
		const TArray<uint8> Prefix(Full.GetData(), 75);
		FVoxelUpdateNotificationMessage Message;
		TestFalse(TEXT("75 bytes is one short of the metadata minimum and is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	// A nine-octet body lets Vx/Vy/Vz/VoxelType read, but leaves no room for the StateSize field's own
	// two. Prefix lengths here are of the whole message, which loses nine octets to the trailer as well
	// as sixty-seven to the header before the body begins.
	{
		const TArray<uint8> Prefix(Full.GetData(), 85);
		FVoxelUpdateNotificationMessage Message;
		TestFalse(TEXT("a body with no room for the StateSize field is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	// Reads StateSize (=5) but leaves only 4 bytes for it, one short of the declared state.
	{
		const TArray<uint8> Prefix(Full.GetData(), 90);
		FVoxelUpdateNotificationMessage Message;
		TestFalse(TEXT("a declared state length that runs one byte past the frame is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	// Leaves exactly the declared 5 state bytes: the accept path with no room left over.
	{
		const TArray<uint8> Prefix(Full.GetData(), 91);
		FVoxelUpdateNotificationMessage Message;
		if (TestTrue(TEXT("a declared state length that exactly fits the frame is accepted"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix)))
		{
			TestTrue(TEXT("bContainsState at the exact-fit boundary"), Message.bContainsState);
			TestEqual(TEXT("StateBytes length at the exact-fit boundary"), Message.StateBytes.Num(), State.Num());
			if (Message.StateBytes.Num() == State.Num())
			{
				TestTrue(TEXT("StateBytes content at the exact-fit boundary"),
					FMemory::Memcmp(Message.StateBytes.GetData(), State.GetData(), State.Num()) == 0);
			}
		}
	}

	return true;
}

// FClientAudioNotification: metadata, SampleRate, NumChannels, FrameCount, then FrameCount frames of
// [FrameSize][AudioData]. The frame loop has two distinct failure shapes worth pinning: a short read on
// a frame's own FrameSize field breaks the loop and still returns true with fewer frames than declared,
// while a FrameSize that overruns the remaining bytes rejects the whole message.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsClientAudioNotificationTest,
	FCrowdyMessageFieldsMiscNoisyTest, "CrowdySDK.Wire.FieldsFClientAudioNotification",
	CrowdyMessageFieldsMiscTestFlags)
bool FCrowdyMessageFieldsClientAudioNotificationTest::RunTest(const FString& Parameters)
{
	const FString Uuid = TEXT("11112222333344445555666677778888");

	TArray<uint8> Full = BuildCrowdyMetadataHeaderBytes(777, 10, 20, -30,
		ECrowdyReplicationDistance::Four_Chunks, ECrowdyDecayRate::No_Decay, false, Uuid);
	Full.Append(USerializationFunctionLibrary::SerializeValue<int32>(48000)); // SampleRate
	Full.Append(USerializationFunctionLibrary::SerializeValue<int32>(2));     // NumChannels
	Full.Append(USerializationFunctionLibrary::SerializeValue<int32>(2));     // FrameCount
	Full.Append(USerializationFunctionLibrary::SerializeValue<int32>(3));     // Frame 0 FrameSize
	Full.Append(TArray<uint8>({ 1, 2, 3 }));                                  // Frame 0 AudioData
	Full.Append(USerializationFunctionLibrary::SerializeValue<int32>(2));     // Frame 1 FrameSize
	Full.Append(TArray<uint8>({ 9, 8 }));                                     // Frame 1 AudioData
	AppendCrowdyTrailerBytes(Full, 999, 5);

	// Positive acceptance: every field the body sets, decoded from a well-formed 101-byte frame.
	{
		FClientAudioNotification Message;
		if (!TestTrue(TEXT("a well-formed audio notification decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Full)))
		{
			return false;
		}

		TestEqual(TEXT("AppID"), Message.AppID, static_cast<int64>(777));
		TestEqual(TEXT("ChunkX"), Message.ChunkX, static_cast<int64>(10));
		TestEqual(TEXT("ChunkY"), Message.ChunkY, static_cast<int64>(20));
		TestEqual(TEXT("ChunkZ"), Message.ChunkZ, static_cast<int64>(-30));
		TestEqual(TEXT("ReplicationDistance"), static_cast<int32>(Message.ReplicationDistance),
			static_cast<int32>(ECrowdyReplicationDistance::Four_Chunks));
		TestEqual(TEXT("DecayRate"), static_cast<int32>(Message.DecayRate),
			static_cast<int32>(ECrowdyDecayRate::No_Decay));
		TestFalse(TEXT("bContainsAuth"), Message.bContainsAuth);
		TestEqual(TEXT("UUID"), Message.UUID, FCrowdyActorId::FromStringOrUnset(Uuid));
		TestEqual(TEXT("Timestamp"), Message.Timestamp, static_cast<int64>(999));
		TestEqual(TEXT("SequenceNumber"), static_cast<int32>(Message.SequenceNumber), 5);

		TestEqual(TEXT("SampleRate"), Message.SampleRate, 48000);
		TestEqual(TEXT("NumChannels"), Message.NumChannels, 2);
		if (TestEqual(TEXT("Frames.Num()"), Message.Frames.Num(), 2))
		{
			TestEqual(TEXT("Frame 0 FrameSize"), Message.Frames[0].FrameSize, 3);
			TestEqual(TEXT("Frame 0 AudioData length"), Message.Frames[0].AudioData.Num(), 3);
			if (Message.Frames[0].AudioData.Num() == 3)
			{
				const uint8 Expected0[] = { 1, 2, 3 };
				TestTrue(TEXT("Frame 0 AudioData content"),
					FMemory::Memcmp(Message.Frames[0].AudioData.GetData(), Expected0, 3) == 0);
			}
			TestEqual(TEXT("Frame 1 FrameSize"), Message.Frames[1].FrameSize, 2);
			TestEqual(TEXT("Frame 1 AudioData length"), Message.Frames[1].AudioData.Num(), 2);
			if (Message.Frames[1].AudioData.Num() == 2)
			{
				const uint8 Expected1[] = { 9, 8 };
				TestTrue(TEXT("Frame 1 AudioData content"),
					FMemory::Memcmp(Message.Frames[1].AudioData.GetData(), Expected1, 2) == 0);
			}
		}
	}

	// One byte short of the metadata minimum.
	{
		const TArray<uint8> Prefix(Full.GetData(), 75);
		FClientAudioNotification Message;
		TestFalse(TEXT("75 bytes is one short of the metadata minimum and is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	// The body is what the header and the trailer leave behind, so a prefix of the whole message loses
	// nine bytes to the trailer as well: 87 bytes leaves a body of 11, one short of the 12 that
	// SampleRate, NumChannels and FrameCount need between them.
	{
		const TArray<uint8> Prefix(Full.GetData(), 87);
		FClientAudioNotification Message;
		TestFalse(TEXT("a body one octet short of the FrameCount field is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	// FrameCount (=2) is readable at exactly a 12-octet body, but no bytes remain for any frame's own
	// FrameSize field. The per-frame loop's floor check breaks rather than fails, so this is a positive
	// decode with an empty Frames array despite FrameCount declaring 2 - a real quirk of today's code,
	// pinned here rather than fixed.
	{
		const TArray<uint8> Prefix(Full.GetData(), 88);
		FClientAudioNotification Message;
		if (TestTrue(TEXT("a body with no room for a frame still decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix)))
		{
			TestEqual(TEXT("Frames.Num() when no frame fits"), Message.Frames.Num(), 0);
		}
	}

	// A frame whose declared FrameSize overruns the bytes actually left in the buffer rejects the whole
	// message: FrameCount=1, FrameSize=10, but only 3 bytes of audio data follow.
	{
		TArray<uint8> Oversized = BuildCrowdyMetadataHeaderBytes(1, 1, 1, 1,
			ECrowdyReplicationDistance::One_Chunk, ECrowdyDecayRate::No_Decay, false, Uuid);
		Oversized.Append(USerializationFunctionLibrary::SerializeValue<int32>(8000));
		Oversized.Append(USerializationFunctionLibrary::SerializeValue<int32>(1));
		Oversized.Append(USerializationFunctionLibrary::SerializeValue<int32>(1));  // FrameCount
		Oversized.Append(USerializationFunctionLibrary::SerializeValue<int32>(10)); // FrameSize, overruns
		Oversized.Append(TArray<uint8>({ 1, 2, 3 }));                               // only 3 bytes follow
		// The trailer has to be here or the body ends nine octets early and the decode fails while reading
		// FrameCount, which would leave the frame-size bound this vector exists for untouched.
		AppendCrowdyTrailerBytes(Oversized, 1, 1);

		FClientAudioNotification Message;
		TestFalse(TEXT("a FrameSize that overruns the remaining bytes is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Oversized));
	}

	return true;
}

// FChannelMessageNotification: a layout of its own, not the spatial header -
// [ChannelId(8)][senderUuid(32, unvalidated)][PayloadLength(2)][Payload][optional Timestamp(8)+Sequence(1)].
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsChannelNotificationTest,
	FCrowdyMessageFieldsMiscNoisyTest, "CrowdySDK.Wire.FieldsFChannelMessageNotification",
	CrowdyMessageFieldsMiscTestFlags)
bool FCrowdyMessageFieldsChannelNotificationTest::RunTest(const FString& Parameters)
{
	const FString SenderUuid = TEXT("ZZZZ1111YYYY2222XXXX3333WWWW4444");
	const TArray<uint8> Payload = { 0xDE, 0xAD, 0xBE, 0xEF };

	TArray<uint8> Full;
	Full.Append(USerializationFunctionLibrary::SerializeValue<int64>(42)); // ChannelId
	{
		const FTCHARToUTF8 ConvertedUuid(*SenderUuid);
		check(ConvertedUuid.Length() == 32);
		Full.Append(reinterpret_cast<const uint8*>(ConvertedUuid.Get()), ConvertedUuid.Length());
	}
	Full.Append(USerializationFunctionLibrary::SerializeValue<uint16>(static_cast<uint16>(Payload.Num())));
	Full.Append(Payload);
	Full.Append(USerializationFunctionLibrary::SerializeValue<int64>(1700000000000LL)); // Timestamp
	Full.Add(9); // SequenceNumber

	// Positive acceptance: every field the body sets, including the optional tail, from a well-formed
	// 55-byte frame.
	{
		FChannelMessageNotification Message;
		if (!TestTrue(TEXT("a well-formed channel notification decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Full)))
		{
			return false;
		}

		TestEqual(TEXT("ChannelId"), Message.ChannelId, static_cast<int64>(42));
		TestEqual(TEXT("UUID"), Message.UUID, FCrowdyActorId::FromStringOrUnset(SenderUuid));
		TestEqual(TEXT("Payload length"), Message.Payload.Num(), Payload.Num());
		if (Message.Payload.Num() == Payload.Num())
		{
			TestTrue(TEXT("Payload content"),
				FMemory::Memcmp(Message.Payload.GetData(), Payload.GetData(), Payload.Num()) == 0);
		}
		TestEqual(TEXT("Timestamp"), Message.Timestamp, static_cast<int64>(1700000000000LL));
		TestEqual(TEXT("SequenceNumber"), static_cast<int32>(Message.SequenceNumber), 9);
	}

	// One byte short of HeaderSize (8 + 32 + 2 = 42): the PayloadLength field itself has no room.
	{
		const TArray<uint8> Prefix(Full.GetData(), 41);
		FChannelMessageNotification Message;
		TestFalse(TEXT("41 bytes is one short of the header and is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	// Exactly 42 bytes reads the real PayloadLength field (=4) but leaves zero bytes for the payload
	// itself, so the declared length runs past the frame.
	{
		const TArray<uint8> Prefix(Full.GetData(), 42);
		FChannelMessageNotification Message;
		TestFalse(TEXT("a declared payload length with no room at all is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	// One byte short of the declared 4-byte payload fitting (needs offset 42 + 4 = 46).
	{
		const TArray<uint8> Prefix(Full.GetData(), 45);
		FChannelMessageNotification Message;
		TestFalse(TEXT("a declared payload length that runs one byte past the frame is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	// The trailer is required: one byte short of the full 9-byte tail (offset 46 + 9 = 55) is refused, so
	// this reader and the vendored decoder agree about which frames are messages. This used to decode with
	// Timestamp and SequenceNumber left at their defaults.
	{
		const TArray<uint8> Prefix(Full.GetData(), 54);
		FChannelMessageNotification Message;
		if (TestFalse(TEXT("a frame one byte short of the tail is refused"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix)))
		{
			TestEqual(TEXT("ChannelId is not written by a refused frame"), Message.ChannelId, static_cast<int64>(0));
			TestEqual(TEXT("Payload is not written by a refused frame"), Message.Payload.Num(), 0);
			TestEqual(TEXT("Timestamp is not written by a refused frame"), Message.Timestamp, static_cast<int64>(0));
			TestEqual(TEXT("SequenceNumber is not written by a refused frame"),
				static_cast<int32>(Message.SequenceNumber), 0);
		}
	}

	return true;
}

// FPingTestMessage: metadata, then a single SendTime field. ReceiveTime is stamped from the wall clock
// at decode time, so it is bracketed against the ticks observed immediately before and after the call
// rather than compared to a literal.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsPingTestMessageTest,
	FCrowdyMessageFieldsMiscNoisyTest, "CrowdySDK.Wire.FieldsFPingTestMessage",
	CrowdyMessageFieldsMiscTestFlags)
bool FCrowdyMessageFieldsPingTestMessageTest::RunTest(const FString& Parameters)
{
	const FString Uuid = TEXT("0123456789abcdef0123456789abcdef");

	TArray<uint8> Full = BuildCrowdyMetadataHeaderBytes(42, 5, -5, 15,
		ECrowdyReplicationDistance::Six_Chunks, ECrowdyDecayRate::Linear_10, false, Uuid);
	const int64 SendTimeValue = 0x0f1e2d3c4b5a6978LL;
	Full.Append(USerializationFunctionLibrary::SerializeValue<int64>(SendTimeValue));
	AppendCrowdyTrailerBytes(Full, 555555, 17);

	// Positive acceptance: every field the body sets, from a well-formed 84-byte frame.
	{
		FPingTestMessage Message;
		const int64 BeforeTicks = FDateTime::UtcNow().GetTicks() / 10000;
		const bool bDecoded = CrowdyWireParity::DecodeStrippedPayload(Message, Full);
		const int64 AfterTicks = FDateTime::UtcNow().GetTicks() / 10000;

		if (!TestTrue(TEXT("a well-formed ping message decodes"), bDecoded))
		{
			return false;
		}

		TestEqual(TEXT("AppID"), Message.AppID, static_cast<int64>(42));
		TestEqual(TEXT("ChunkX"), Message.ChunkX, static_cast<int64>(5));
		TestEqual(TEXT("ChunkY"), Message.ChunkY, static_cast<int64>(-5));
		TestEqual(TEXT("ChunkZ"), Message.ChunkZ, static_cast<int64>(15));
		TestEqual(TEXT("ReplicationDistance"), static_cast<int32>(Message.ReplicationDistance),
			static_cast<int32>(ECrowdyReplicationDistance::Six_Chunks));
		TestEqual(TEXT("DecayRate"), static_cast<int32>(Message.DecayRate),
			static_cast<int32>(ECrowdyDecayRate::Linear_10));
		TestFalse(TEXT("bContainsAuth"), Message.bContainsAuth);
		TestEqual(TEXT("UUID"), Message.UUID, FCrowdyActorId::FromStringOrUnset(Uuid));
		TestEqual(TEXT("Timestamp"), Message.Timestamp, static_cast<int64>(555555));
		TestEqual(TEXT("SequenceNumber"), static_cast<int32>(Message.SequenceNumber), 17);

		TestEqual(TEXT("SendTime"), Message.SendTime, SendTimeValue);
		TestTrue(TEXT("ReceiveTime is stamped within the call's own wall-clock bracket"),
			Message.ReceiveTime >= BeforeTicks && Message.ReceiveTime <= AfterTicks);
	}

	// One byte short of the metadata minimum.
	{
		const TArray<uint8> Prefix(Full.GetData(), 75);
		FPingTestMessage Message;
		TestFalse(TEXT("75 bytes is one short of the metadata minimum and is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	// One octet short of an eight-octet body, which is what SendTime needs and all this layout carries.
	// The whole message therefore has to run to 67 + 8 + 9 before it decodes.
	{
		const TArray<uint8> Prefix(Full.GetData(), 83);
		FPingTestMessage Message;
		TestFalse(TEXT("a body one octet short of SendTime is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	{
		const TArray<uint8> Prefix(Full.GetData(), 84);
		FPingTestMessage Message;
		TestTrue(TEXT("a body of exactly SendTime is accepted"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Prefix));
	}

	return true;
}

// FDefaultMessage::DecodePayload is `return false;` unconditionally - it never reads a byte and can never
// succeed. It exists as the parser's BAD_MESSAGE placeholder and is never constructed via DecodePayload on
// the receive path (the parser builds it directly for empty datagrams and unrecognized opcodes). There is
// deliberately no positive-acceptance vector for this type: none exists for the current code to produce.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsDefaultMessageTest,
	"CrowdySDK.Wire.FieldsFDefaultMessage", CrowdyMessageFieldsMiscTestFlags)
bool FCrowdyMessageFieldsDefaultMessageTest::RunTest(const FString& Parameters)
{
	{
		FDefaultMessage Message;
		TestFalse(TEXT("an empty buffer is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, TArray<uint8>()));
	}
	{
		const TArray<uint8> Data = { 1, 2, 3 };
		FDefaultMessage Message;
		TestFalse(TEXT("an arbitrary non-empty buffer is still rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
