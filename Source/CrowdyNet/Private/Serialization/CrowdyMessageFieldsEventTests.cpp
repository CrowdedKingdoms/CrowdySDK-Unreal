#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Messages/GameObjects/FGameObjectActivationNotification.h"
#include "Messages/GameObjects/FServerEventNotification.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "CrowdyWireParitySupport.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyMessageFieldsEventTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Several reject paths exercised below log at Error level: a failed ensure while reading the header,
	// and an event-state payload whose type id does not resolve to a registered struct. The assertions
	// in each test are the actual signal; this only stops that expected noise from failing the test on
	// its own.
	class FCrowdyMessageFieldsEventNoisyTest : public FAutomationTestBase
	{
	public:
		FCrowdyMessageFieldsEventNoisyTest(const FString& InName, const bool bInComplexTask)
			: FAutomationTestBase(InName, bInComplexTask)
		{
		}

		virtual bool SuppressLogErrors() override { return true; }
	};

	void EventFieldsTestAppendAscii(TArray<uint8>& Out, const ANSICHAR* Text)
	{
		for (const ANSICHAR* Cursor = Text; *Cursor; ++Cursor)
		{
			Out.Add(static_cast<uint8>(*Cursor));
		}
	}

	// The 67-byte block every spatial message reads first, in the order it is read: four int64
	// chunk-coordinate fields, three flag bytes, then a 32-character ASCII actor id.
	void EventFieldsTestAppendMetadataHeader(TArray<uint8>& Out, int64 AppID, int64 ChunkX, int64 ChunkY,
		int64 ChunkZ, uint8 ReplicationDistanceRaw, uint8 DecayRateRaw, uint8 ContainsAuthRaw,
		const ANSICHAR* Uuid32)
	{
		Out.Append(USerializationFunctionLibrary::SerializeValue<int64>(AppID));
		Out.Append(USerializationFunctionLibrary::SerializeValue<int64>(ChunkX));
		Out.Append(USerializationFunctionLibrary::SerializeValue<int64>(ChunkY));
		Out.Append(USerializationFunctionLibrary::SerializeValue<int64>(ChunkZ));
		Out.Add(ReplicationDistanceRaw);
		Out.Add(DecayRateRaw);
		Out.Add(ContainsAuthRaw);
		EventFieldsTestAppendAscii(Out, Uuid32);
	}
}

// FGameEventNotification::DecodePayload (also the body FSingleActorNotification inherits unchanged,
// and the layout FServerEventNotification's own override deliberately does NOT reuse).
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsGameEventNotificationTest,
	FCrowdyMessageFieldsEventNoisyTest, "CrowdySDK.Wire.FieldsFGameEventNotification",
	CrowdyMessageFieldsEventTestFlags)
bool FCrowdyMessageFieldsGameEventNotificationTest::RunTest(const FString& Parameters)
{
	const ANSICHAR* Uuid = "11112222333344445555666677778888";

	// The same octets as the id type the message carries, taken from the literal above rather than
	// restated, so the vector and the expectation cannot drift apart.
	const FCrowdyActorId ExpectedActorId = FCrowdyActorId::FromOctets(
		TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Uuid), FCrowdyActorId::NumOctets));

	// Vector A: 82 bytes, the shortest well-formed message whose fields do not overlap: a 67-byte
	// metadata block, EventType at 67-68, StateSize at 69-72, a zero-length state, no envelope, and
	// a 9-byte tail of its own. Note that the tail is read from the last nine bytes whatever the
	// length, so at the 76-byte floor the decoder enforces it would overlap EventType and StateSize and
	// the message would still be accepted. Vector D is the boundary that rejects.
	{
		TArray<uint8> Data;
		EventFieldsTestAppendMetadataHeader(Data, 1111, -22, 33, -44, 5, 2, 0, Uuid);
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x1234)); // EventType, offset 67-68
		Data.Append(USerializationFunctionLibrary::SerializeValue<int32>(0));       // StateSize, offset 69-72
		// Tail is anchored to the last 9 bytes of the buffer regardless of the fields above it.
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(999999999)); // Timestamp
		Data.Add(7);                                                                  // SequenceNumber
		TestEqual(TEXT("vector A carries every field without overlap"), Data.Num(), 82);

		FGameEventNotification Message;
		if (TestTrue(TEXT("vector A (82 bytes, zero-length state) decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
		{
			TestEqual(TEXT("A: AppID"), Message.AppID, static_cast<int64>(1111));
			TestEqual(TEXT("A: ChunkX"), Message.ChunkX, static_cast<int64>(-22));
			TestEqual(TEXT("A: ChunkY"), Message.ChunkY, static_cast<int64>(33));
			TestEqual(TEXT("A: ChunkZ"), Message.ChunkZ, static_cast<int64>(-44));
			TestEqual(TEXT("A: ReplicationDistance"), static_cast<int32>(Message.ReplicationDistance), 5);
			TestEqual(TEXT("A: DecayRate"), static_cast<int32>(Message.DecayRate), 2);
			TestFalse(TEXT("A: bContainsAuth"), Message.bContainsAuth);
			TestEqual(TEXT("A: UUID"), Message.UUID, ExpectedActorId);
			TestEqual(TEXT("A: Timestamp"), Message.Timestamp, static_cast<int64>(999999999));
			TestEqual(TEXT("A: SequenceNumber"), static_cast<int32>(Message.SequenceNumber), 7);
			TestEqual(TEXT("A: EventType"), static_cast<int32>(Message.EventType), 0x1234);
			TestEqual(TEXT("A: StateSize"), Message.StateSize, 0);
			TestEqual(TEXT("A: StateBytes length"), Message.StateView.Num(), 0);
			TestFalse(TEXT("A: State does not resolve from an empty payload"), Message.State.IsValid());
			// No envelope bytes were present, so the routing fields stay at their broadcast defaults.
			TestEqual(TEXT("A: Target defaults to Everyone"), static_cast<int32>(Message.Target),
				static_cast<int32>(ECrowdyTarget::Everyone));
			TestFalse(TEXT("A: TargetID stays unset"), Message.TargetID.IsValid());
		}
	}

	// Vector B: a non-empty state and still no envelope (buffer ends before the 33-byte
	// Target+TargetID envelope window opens), so a pre-envelope sender's frame decodes the same
	// way it always has.
	{
		TArray<uint8> Data;
		EventFieldsTestAppendMetadataHeader(Data, 64, -1, 2, -3, 8, 0, 0, Uuid);
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x0abc)); // EventType, offset 67-68
		Data.Append(USerializationFunctionLibrary::SerializeValue<int32>(4));       // StateSize, offset 69-72
		const uint8 State[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
		Data.Append(State, 4);                                                      // StateBytes, offset 73-76
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(42424242)); // Timestamp
		Data.Add(9);                                                                 // SequenceNumber
		TestEqual(TEXT("vector B length"), Data.Num(), 86);

		FGameEventNotification Message;
		if (TestTrue(TEXT("vector B (non-empty state, no envelope) decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
		{
			TestEqual(TEXT("B: EventType"), static_cast<int32>(Message.EventType), 0x0abc);
			TestEqual(TEXT("B: StateSize"), Message.StateSize, 4);
			TestEqual(TEXT("B: StateBytes length"), Message.StateView.Num(), 4);
			if (Message.StateView.Num() == 4)
			{
				TestTrue(TEXT("B: StateBytes content"), FMemory::Memcmp(Message.StateView.GetData(), State, 4) == 0);
			}
			// The state bytes above do not resolve to a registered struct, so the decode still
			// reports success (DecodePayload returns true) but leaves State invalid.
			TestFalse(TEXT("B: State does not resolve"), Message.State.IsValid());
			TestEqual(TEXT("B: Target defaults to Everyone"), static_cast<int32>(Message.Target),
				static_cast<int32>(ECrowdyTarget::Everyone));
			TestFalse(TEXT("B: TargetID stays unset"), Message.TargetID.IsValid());
		}
	}

	// Vector C: the same header and state as vector B, with the 33-byte Target+TargetID envelope
	// appended after the state (a 1-byte target enum, then a 32-character hex TargetID) before the
	// tail. This is the only vector in this test that reaches those two fields.
	{
		TArray<uint8> Data;
		EventFieldsTestAppendMetadataHeader(Data, 64, -1, 2, -3, 8, 0, 0, Uuid);
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x0abc));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int32>(4));
		const uint8 State[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
		Data.Append(State, 4);
		Data.Add(static_cast<uint8>(ECrowdyTarget::Owner)); // Target byte
		EventFieldsTestAppendAscii(Data, "aaaabbbbccccddddeeeeffff00001111"); // TargetID, 32 hex chars
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(888888888)); // Timestamp
		Data.Add(42);                                                                 // SequenceNumber
		TestEqual(TEXT("vector C length"), Data.Num(), 119);

		FGameEventNotification Message;
		if (TestTrue(TEXT("vector C (with envelope) decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
		{
			TestEqual(TEXT("C: Target"), static_cast<int32>(Message.Target),
				static_cast<int32>(ECrowdyTarget::Owner));
			const FGuid ExpectedTargetID(0xaaaabbbb, 0xccccdddd, 0xeeeeffff, 0x00001111);
			TestTrue(TEXT("C: TargetID"), Message.TargetID == ExpectedTargetID);
			TestEqual(TEXT("C: Timestamp"), Message.Timestamp, static_cast<int64>(888888888));
			TestEqual(TEXT("C: SequenceNumber"), static_cast<int32>(Message.SequenceNumber), 42);
		}
	}

	// Vector C2: the same envelope as vector C, but the target id holds octets that are not text. The
	// key is derived from the 32 octets themselves, so a component holding anything outside the
	// hexadecimal alphabet reads as the largest value and the other three still read as themselves.
	//
	// This is the vector that tells the two derivations apart. Reading the same octets as a string
	// first loses them: 0xff cannot begin a UTF-8 sequence, the continuation octets behind it are
	// swallowed into a single replacement character, and what comes back is no longer 32 octets, so a
	// text-based derivation yields no key at all. A hexadecimal target id cannot show that difference,
	// which is why vector C alone cannot hold this property.
	{
		TArray<uint8> Data;
		EventFieldsTestAppendMetadataHeader(Data, 64, -1, 2, -3, 8, 0, 0, Uuid);
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x0abc));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int32>(4));
		const uint8 State[4] = { 0xaa, 0xbb, 0xcc, 0xdd };
		Data.Append(State, 4);
		Data.Add(static_cast<uint8>(ECrowdyTarget::Entity)); // Target byte
		EventFieldsTestAppendAscii(Data, "aaaabbbbccccddddeeeeffff00"); // 26 octets of the target id
		const uint8 NonTextTail[6] = { 0xff, 0x80, 0x81, 0x82, 0x83, 0x84 };
		Data.Append(NonTextTail, 6);                                   // the last 6, not text
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(777777777)); // Timestamp
		Data.Add(7);                                                                  // SequenceNumber
		TestEqual(TEXT("vector C2 length"), Data.Num(), 119);

		FGameEventNotification Message;
		if (TestTrue(TEXT("vector C2 (target id that is not text) decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
		{
			TestEqual(TEXT("C2: Target"), static_cast<int32>(Message.Target),
				static_cast<int32>(ECrowdyTarget::Entity));
			const FGuid ExpectedTargetID(0xaaaabbbb, 0xccccdddd, 0xeeeeffff, 0xffffffff);
			TestTrue(TEXT("C2: TargetID is derived from the octets"), Message.TargetID == ExpectedTargetID);
			TestTrue(TEXT("C2: TargetID is a usable key"), Message.TargetID.IsValid());
		}
	}

	// Vector D (reject): one byte short of the 76-byte minimum a spatial frame has to clear. The frame is
	// refused before the decoder is entered, so every member stays at its compile-time default.
	{
		TArray<uint8> Data;
		EventFieldsTestAppendMetadataHeader(Data, 1111, -22, 33, -44, 5, 2, 0, Uuid);
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x1234));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int32>(0));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(999999999));
		Data.Add(7);
		Data.SetNum(75); // one short of the 76-byte minimum the decoder enforces
		TestEqual(TEXT("vector D length"), Data.Num(), 75);

		FGameEventNotification Message;
		TestFalse(TEXT("D: one byte short of the metadata minimum is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data));
		TestEqual(TEXT("D: AppID stays at its default (never read)"), Message.AppID, static_cast<int64>(0));
		TestEqual(TEXT("D: EventType stays at its default"), static_cast<int32>(Message.EventType), 0);
	}

	// Vector E (reject): the header, EventType and StateSize all read successfully, but the declared
	// StateSize claims more bytes than the body holds. This is a genuine partial-mutation case: AppID
	// and the rest are already written from the envelope before the state-size check fails, and only
	// StateBytes is explicitly cleared.
	{
		TArray<uint8> Data;
		EventFieldsTestAppendMetadataHeader(Data, 55, 6, 7, 8, 1, 1, 0, Uuid);
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x2222));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int32>(10)); // StateSize claims 10 bytes
		while (Data.Num() < 83)
		{
			Data.Add(0); // only 1 byte actually follows StateSize, not the 10 declared
		}
		TestEqual(TEXT("vector E length"), Data.Num(), 83);

		FGameEventNotification Message;
		TestFalse(TEXT("E: an overrunning StateSize is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data));
		TestEqual(TEXT("E: AppID was already written by the metadata block"), Message.AppID, static_cast<int64>(55));
		TestEqual(TEXT("E: StateSize was already written"), Message.StateSize, 10);
		TestEqual(TEXT("E: StateBytes is explicitly cleared on this reject path"), Message.StateView.Num(), 0);
	}

	return true;
}

// FServerEventNotification::DecodePayload. This is its own override (opcode 139's wire layout is not
// the relayed-client-event layout FGameEventNotification reads), so no envelope is applied here and
// the shared ReplicationDistance/DecayRate/UUID/Timestamp/SequenceNumber members are simply never
// touched by this type.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsServerEventNotificationTest,
	FCrowdyMessageFieldsEventNoisyTest, "CrowdySDK.Wire.FieldsFServerEventNotification",
	CrowdyMessageFieldsEventTestFlags)
bool FCrowdyMessageFieldsServerEventNotificationTest::RunTest(const FString& Parameters)
{
	// Vector A: bContainsAuth true, a 10-byte state, and a 32-byte HMAC region between the state
	// and the trailer. Bytes 32-33 (the ReplicationDistance/DecayRate slots in the shared layout)
	// and 35-66 (the two-GUID identity region) are filled with a non-zero pattern specifically to
	// prove this override never reads them into anything; likewise the 9 trailer bytes.
	{
		TArray<uint8> Data;
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(5000));  // AppID, offset 0-7
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(10));    // ChunkX, offset 8-15
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(-20));   // ChunkY, offset 16-23
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(30));    // ChunkZ, offset 24-31
		Data.Add(0xee); // offset 32, unused by this override
		Data.Add(0xee); // offset 33, unused by this override
		Data.Add(1);    // offset 34: bContainsAuth = true
		for (int32 Index = 0; Index < 32; ++Index)
		{
			Data.Add(0xcd); // offset 35-66, unused identity region
		}
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x5678)); // EventType, offset 67-68
		const uint8 State[10] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19 };
		Data.Append(State, 10); // offset 69-78
		for (int32 Index = 0; Index < 32; ++Index)
		{
			Data.Add(0xff); // offset 79-110, HMAC region: present because bContainsAuth is true, unread
		}
		for (int32 Index = 1; Index <= 9; ++Index)
		{
			Data.Add(static_cast<uint8>(Index)); // offset 111-119, trailer: unread by this override
		}
		TestEqual(TEXT("vector A length"), Data.Num(), 120);

		FServerEventNotification Message;
		if (TestTrue(TEXT("vector A (signed, 10-byte state) decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
		{
			TestEqual(TEXT("A: AppID"), Message.AppID, static_cast<int64>(5000));
			TestEqual(TEXT("A: ChunkX"), Message.ChunkX, static_cast<int64>(10));
			TestEqual(TEXT("A: ChunkY"), Message.ChunkY, static_cast<int64>(-20));
			TestEqual(TEXT("A: ChunkZ"), Message.ChunkZ, static_cast<int64>(30));
			TestTrue(TEXT("A: bContainsAuth"), Message.bContainsAuth);
			TestEqual(TEXT("A: EventType"), static_cast<int32>(Message.EventType), 0x5678);
			TestEqual(TEXT("A: StateBytes length"), Message.StateView.Num(), 10);
			// This carrier derives its state length from the frame rather than reading a declared one, so
			// the inherited StateSize is the field most likely to be left behind. A reader that trusts it
			// must see the same number the view carries.
			TestEqual(TEXT("A: StateSize agrees with the view it describes"), Message.StateSize, 10);
			if (Message.StateView.Num() == 10)
			{
				TestTrue(TEXT("A: StateBytes content"), FMemory::Memcmp(Message.StateView.GetData(), State, 10) == 0);
			}
			// The base class's State (an FInstancedStruct) is never populated: this carrier's state
			// is raw application bytes, so State.Reset() runs unconditionally.
			TestFalse(TEXT("A: State is always reset, never decoded"), Message.State.IsValid());
			// Fields the shared envelope would normally fill are left at their class defaults, because
			// this override reads the layout itself and never applies an envelope.
			TestEqual(TEXT("A: ReplicationDistance stays at its default"),
				static_cast<int32>(Message.ReplicationDistance), static_cast<int32>(ECrowdyReplicationDistance::Eight_Chunks));
			TestEqual(TEXT("A: DecayRate stays at its default"),
				static_cast<int32>(Message.DecayRate), static_cast<int32>(ECrowdyDecayRate::No_Decay));
			TestFalse(TEXT("A: UUID stays unset"), Message.UUID.IsSet());
			TestEqual(TEXT("A: Timestamp stays at its default"), Message.Timestamp, static_cast<int64>(0));
			TestEqual(TEXT("A: SequenceNumber stays at its default"), static_cast<int32>(Message.SequenceNumber), 0);
		}
	}

	// Vector B: bContainsAuth false, so no HMAC region sits between the state and the trailer.
	{
		TArray<uint8> Data;
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(-77));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(2));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(3));
		Data.Add(0);
		Data.Add(0);
		Data.Add(0); // offset 34: bContainsAuth = false
		for (int32 Index = 0; Index < 32; ++Index)
		{
			Data.Add(0);
		}
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x0001));
		const uint8 State[5] = { 0xee, 0xee, 0xee, 0xee, 0xee };
		Data.Append(State, 5); // offset 69-73
		for (int32 Index = 0; Index < 9; ++Index)
		{
			Data.Add(0); // trailer, unread
		}
		TestEqual(TEXT("vector B length"), Data.Num(), 83);

		FServerEventNotification Message;
		if (TestTrue(TEXT("vector B (unsigned, 5-byte state) decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
		{
			TestEqual(TEXT("B: AppID"), Message.AppID, static_cast<int64>(-77));
			TestFalse(TEXT("B: bContainsAuth"), Message.bContainsAuth);
			TestEqual(TEXT("B: EventType"), static_cast<int32>(Message.EventType), 0x0001);
			TestEqual(TEXT("B: StateBytes length"), Message.StateView.Num(), 5);
			TestEqual(TEXT("B: StateSize agrees with the view it describes"), Message.StateSize, 5);
			if (Message.StateView.Num() == 5)
			{
				TestTrue(TEXT("B: StateBytes content"), FMemory::Memcmp(Message.StateView.GetData(), State, 5) == 0);
			}
		}
	}

	// Vector C: the exact 78-byte minimum (StateOffset 69 + TrailerBytes 9) with bContainsAuth
	// false, so the derived state length is exactly zero. This is the boundary the accept path
	// actually depends on, as opposed to vector D just below it.
	{
		TArray<uint8> Data;
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1));
		Data.Add(0);
		Data.Add(0);
		Data.Add(0); // bContainsAuth = false
		for (int32 Index = 0; Index < 32; ++Index)
		{
			Data.Add(0);
		}
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x00ff));
		for (int32 Index = 0; Index < 9; ++Index)
		{
			Data.Add(0); // trailer, unread
		}
		TestEqual(TEXT("vector C length"), Data.Num(), 78);

		FServerEventNotification Message;
		if (TestTrue(TEXT("vector C (78-byte minimum, zero-length state) decodes"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
		{
			TestEqual(TEXT("C: EventType"), static_cast<int32>(Message.EventType), 0x00ff);
			TestEqual(TEXT("C: StateBytes length"), Message.StateView.Num(), 0);
			TestEqual(TEXT("C: StateSize agrees with the view it describes"), Message.StateSize, 0);
		}
	}

	// Vector D (reject): 77 bytes, one short of the 78-byte floor the function checks up front.
	{
		TArray<uint8> Data;
		Data.SetNumZeroed(77);
		FServerEventNotification Message;
		TestFalse(TEXT("D: one byte short of the 78-byte floor is rejected"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data));
		TestEqual(TEXT("D: EventType stays at its default"), static_cast<int32>(Message.EventType), 0);
	}

	// Vector E (reject, a pinned gap rather than a fix): the 78-byte floor this function checks
	// does not account for the 32-byte HMAC region a signed (bContainsAuth true) frame also needs,
	// so a 78-byte signed frame passes that first check and only then computes a negative state
	// length (78 - 9 - 32 - 69 = -32) and is rejected by the second check instead. Both checks
	// reject this particular input; what this pins is that the first one is not sufficient on its
	// own for a signed frame, since it is silently missing 32 bytes of room for the HMAC.
	{
		TArray<uint8> Data;
		Data.SetNumZeroed(78);
		Data[34] = 1; // bContainsAuth = true
		FServerEventNotification Message;
		TestFalse(TEXT("E: a 78-byte signed frame is rejected by the derived-state-length check"),
			CrowdyWireParity::DecodeStrippedPayload(Message, Data));
	}

	return true;
}

// FGameObjectActivationNotification::DecodePayload. Its individual DeserializeValue calls still discard
// their return values, but every one of them is now covered by a single length check on the fixed block
// ahead of the state, and the state copy is bounded by the octets behind the declared length and written
// into an array sized for it. Vectors C and D below are the ones that could not previously be written:
// a non-zero state used to be copied into an array nothing had sized, which crashes the test process
// rather than failing one test.
//
// The vectors below hand the decoder the whole message rather than letting an envelope be taken off
// first, because this type declares the client-event opcode while reading a layout of its own from
// offset zero. Nothing routes it, so the two never meet in production.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsGameObjectActivationNotificationTest,
	"CrowdySDK.Wire.FieldsFGameObjectActivationNotification", CrowdyMessageFieldsEventTestFlags)
bool FCrowdyMessageFieldsGameObjectActivationNotificationTest::RunTest(const FString& Parameters)
{
	// Vector A (reject): an empty buffer. This is the only bounds check the function has.
	{
		TArray<uint8> Data;
		FGameObjectActivationNotification Message;
		TestFalse(TEXT("A: an empty buffer is rejected"),
			CrowdyWireParity::DecodeWholePayload(Message, Data));
	}

	// Vector B: the full 70-byte fixed layout (four int64 fields, a 32-character uuid, a uint16
	// event type, then a StateSize of exactly zero). Every read here has enough bytes behind it to
	// actually succeed, and a StateSize of zero is the only value that makes the unsized final
	// Memcpy safe to execute.
	{
		TArray<uint8> Data;
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(4242)); // MapID, offset 0-7
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1));    // ChunkX, offset 8-15
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(2));    // ChunkY, offset 16-23
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(3));    // ChunkZ, offset 24-31
		EventFieldsTestAppendAscii(Data, "11112222333344445555666677778888");    // ActivatorUUID, offset 32-63
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x00aa)); // EventType, offset 64-65
		Data.Append(USerializationFunctionLibrary::SerializeValue<int32>(0));       // StateSize, offset 66-69
		TestEqual(TEXT("vector B length"), Data.Num(), 70);

		FGameObjectActivationNotification Message;
		if (TestTrue(TEXT("vector B (70-byte layout, zero-length state) decodes"),
			CrowdyWireParity::DecodeWholePayload(Message, Data)))
		{
			TestEqual(TEXT("B: MapID"), Message.MapID, static_cast<int64>(4242));
			TestEqual(TEXT("B: ChunkX"), Message.ChunkX, static_cast<int64>(1));
			TestEqual(TEXT("B: ChunkY"), Message.ChunkY, static_cast<int64>(2));
			TestEqual(TEXT("B: ChunkZ"), Message.ChunkZ, static_cast<int64>(3));
			TestEqual(TEXT("B: ActivatorUUID"), Message.ActivatorUUID,
				FString(TEXT("11112222333344445555666677778888")));
			TestEqual(TEXT("B: EventType"), static_cast<int32>(Message.EventType), 0x00aa);
			TestEqual(TEXT("B: StateSize"), Message.StateSize, 0);
			TestEqual(TEXT("B: StateBytes stays empty"), Message.StateBytes.Num(), 0);
		}
	}

	// Vector C: the same layout with a five-octet state actually present behind the declared length.
	// The state is what the copy writes, so this is the vector that says the destination is sized from
	// the length rather than left at whatever the array happened to hold.
	{
		const uint8 State[5] = { 0xde, 0xad, 0xbe, 0xef, 0x01 };

		TArray<uint8> Data;
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(9));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(8));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(7));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(6));
		EventFieldsTestAppendAscii(Data, "aaaabbbbccccddddeeeeffff00001111");
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x1234));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int32>(5));
		Data.Append(State, 5);
		TestEqual(TEXT("vector C length"), Data.Num(), 75);

		FGameObjectActivationNotification Message;
		if (TestTrue(TEXT("vector C (70-byte layout plus a five-octet state) decodes"),
			CrowdyWireParity::DecodeWholePayload(Message, Data)))
		{
			TestEqual(TEXT("C: StateSize"), Message.StateSize, 5);
			TestEqual(TEXT("C: StateBytes length"), Message.StateBytes.Num(), 5);
			if (Message.StateBytes.Num() == 5)
			{
				TestTrue(TEXT("C: StateBytes content"),
					FMemory::Memcmp(Message.StateBytes.GetData(), State, 5) == 0);
			}
		}
	}

	// Vector D (reject): a declared state length one octet longer than the frame carries. Compared
	// against bytes remaining rather than by adding the length to the offset, so a forged length near
	// the integer ceiling is refused by the same test.
	{
		TArray<uint8> Data;
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int64>(1));
		EventFieldsTestAppendAscii(Data, "11112222333344445555666677778888");
		Data.Append(USerializationFunctionLibrary::SerializeValue<uint16>(0x0001));
		Data.Append(USerializationFunctionLibrary::SerializeValue<int32>(5)); // declares 5
		Data.Append(USerializationFunctionLibrary::SerializeValue<int32>(0)); // carries 4
		TestEqual(TEXT("vector D length"), Data.Num(), 74);

		FGameObjectActivationNotification Message;
		TestFalse(TEXT("D: a state length one octet past the end of the frame is rejected"),
			CrowdyWireParity::DecodeWholePayload(Message, Data));
	}

	// Vector E (reject): one octet short of the 70-byte fixed block, which is the check that keeps every
	// field read below it inside the frame.
	{
		TArray<uint8> Data;
		Data.SetNumZeroed(69);
		FGameObjectActivationNotification Message;
		TestFalse(TEXT("E: one byte short of the 70-byte fixed block is rejected"),
			CrowdyWireParity::DecodeWholePayload(Message, Data));
	}

	// Vector F (reject): 40 octets, too short for the activator id to be read at all. This test declares
	// no expected error, so it is also what says the fixed-block check refuses BEFORE any field read is
	// attempted: reaching the id read on this frame reports it, and an unexpected report fails the test.
	{
		TArray<uint8> Data;
		Data.SetNumZeroed(40);
		FGameObjectActivationNotification Message;
		TestFalse(TEXT("F: a frame too short to hold the activator id is rejected"),
			CrowdyWireParity::DecodeWholePayload(Message, Data));
		TestEqual(TEXT("F: ActivatorUUID is never read"), Message.ActivatorUUID, FString());
	}

	return true;
}

#endif
