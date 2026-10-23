#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Messages/GameObjects/FSingleActorMessage.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/CrowdyNetActorStateWireTestTypes.h"
#include "CrowdyWireParitySupport.h"

// Characterization tests for the actor family of ICrowdyMessage::DecodePayload bodies, pinning what the
// code does today so a later change to how a frame reaches these bodies (a non-owning view instead of a
// freshly sliced TArray<uint8>) can be checked against unchanged behavior. Every vector below is a fixed
// byte layout built by hand from reading the bodies under test; nothing here calls Serialize() to build
// its own input, so a bug shared between Serialize and DecodePayload cannot hide from these tests the way
// it would in a round trip.
namespace
{
	constexpr EAutomationTestFlags CrowdyMessageFieldsActorTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A vector that runs past the end of its own buffer makes the shared deserialization helpers report at
	// Error level, which fails a running test unless it is whitelisted, and some of that output is a
	// multi-line block that AddExpectedError cannot match line by line. Those tests suppress log errors
	// outright instead, matching the convention already in CrowdyWireDecodeParityTests.cpp; every
	// TestEqual/TestTrue call still runs and still fails normally, only the log is not asserted on.
	class FCrowdyMessageFieldsActorNoisyTest : public FAutomationTestBase
	{
	public:
		FCrowdyMessageFieldsActorNoisyTest(const FString& InName, const bool bInComplexTask)
			: FAutomationTestBase(InName, bInComplexTask)
		{
		}

		virtual bool SuppressLogErrors() override { return true; }
	};

	template <typename T>
	void AppendLE(TArray<uint8>& Out, T Value)
	{
		const int32 Base = Out.Num();
		Out.SetNumUninitialized(Base + sizeof(T));
		FMemory::Memcpy(Out.GetData() + Base, &Value, sizeof(T));
	}

	// A 32-character hex string. Used both as the wire actor id (the header reserves exactly 32 octets for
	// it, with no terminator) and, in the envelope test, as a TargetID.
	const FString ActorFieldsGoldenUuid = TEXT("0123456789abcdef0123456789abcdef");
	const FString ActorFieldsTargetIdHex = TEXT("abcdefabcdefabcdefabcdefabcdef12");

	// The same value as the id type the messages actually carry, so the assertions compare octets rather
	// than a decoded string.
	const FCrowdyActorId ActorFieldsGoldenActorId =
		FCrowdyActorId::FromAnsiLiteral("0123456789abcdef0123456789abcdef");

	// Builds the MetadataSize-byte spatial header, in the exact field order it is read: AppID, ChunkX,
	// ChunkY, ChunkZ (int64 each), ReplicationDistance, DecayRate, bContainsAuth (uint8 each), then the
	// 32-character UUID.
	//
	// Every vector in this file passes 0 for bContainsAuth and appends no signature, so they describe
	// unsigned frames, which the protocol permits and which is what keeps their lengths readable. The
	// signed layout, where 32 signature octets sit between the body and the trailer, has its own vector
	// at the end of this file.
	TArray<uint8> BuildActorFieldsMetadata(int64 AppID, int64 ChunkX, int64 ChunkY, int64 ChunkZ,
		uint8 ReplicationDistance, uint8 DecayRate, uint8 bContainsAuth, const FString& Uuid32)
	{
		TArray<uint8> Out;
		AppendLE(Out, AppID);
		AppendLE(Out, ChunkX);
		AppendLE(Out, ChunkY);
		AppendLE(Out, ChunkZ);
		Out.Add(ReplicationDistance);
		Out.Add(DecayRate);
		Out.Add(bContainsAuth);

		const FTCHARToUTF8 Converted(*Uuid32);
		check(Converted.Length() == 32);
		Out.Append(reinterpret_cast<const uint8*>(Converted.Get()), 32);

		check(Out.Num() == MetadataSize);
		return Out;
	}

	// Appends the TailSize-byte trailer, anchored to the end of whatever buffer it is eventually given:
	// an int64 Timestamp then a single SequenceNumber byte.
	void AppendActorFieldsTail(TArray<uint8>& Out, int64 Timestamp, uint8 SequenceNumber)
	{
		AppendLE(Out, Timestamp);
		Out.Add(SequenceNumber);
		check(TailSize == sizeof(int64) + sizeof(uint8));
	}

	bool ActorFieldsBytesEqual(const TConstArrayView<uint8> A, const TConstArrayView<uint8> B)
	{
		return A.Num() == B.Num() && (A.Num() == 0 || FMemory::Memcmp(A.GetData(), B.GetData(), A.Num()) == 0);
	}
}

// FActorUpdateNotificationMessage: a well-formed frame decodes every field the envelope carries plus
// StateSize/StateBytes/PayloadTypeID, and leaves State invalid because the state's leading type tag does
// not resolve to a registered struct (DeserializeActorState fails silently on an unresolved TypeID; see
// SerializationFunctionLibrary.cpp, the failure there logs nothing, unlike the event-state sibling below).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsActorNotificationAcceptTest,
	"CrowdySDK.Wire.FieldsFActorUpdateNotificationMessage.Accept", CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsActorNotificationAcceptTest::RunTest(const FString& Parameters)
{
	// State bytes: the actor-state framing naming a deliberately unregistered TypeID (0xFFFF), followed
	// by 4 arbitrary body bytes. The tag has to be readable for PayloadTypeID to be reported back, and
	// unregistered so State itself stays invalid: this test is about the message envelope's fields, not
	// about decoding a body.
	TArray<uint8> State;
	AppendActorStateFraming(State, static_cast<FCrowdyTypeID>(0xFFFF), CrowdyActorStateWireTestClassID);
	State.Append({ 0xAA, 0xBB, 0xCC, 0xDD });
	const int32 StateSize = State.Num();

	TArray<uint8> Data = BuildActorFieldsMetadata(
		111222333444LL, -5, 6, -7, 3, 2, 0, ActorFieldsGoldenUuid);
	AppendLE(Data, StateSize);
	Data.Append(State);
	AppendActorFieldsTail(Data, 987654321LL, 42);

	FActorUpdateNotificationMessage Message;
	if (!TestTrue(TEXT("accepts a well-formed frame"), CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
	{
		return false;
	}

	// Fields the envelope sets on the base interface.
	TestEqual(TEXT("AppID"), Message.AppID, static_cast<int64>(111222333444LL));
	TestEqual(TEXT("ChunkX"), Message.ChunkX, static_cast<int64>(-5));
	TestEqual(TEXT("ChunkY"), Message.ChunkY, static_cast<int64>(6));
	TestEqual(TEXT("ChunkZ"), Message.ChunkZ, static_cast<int64>(-7));
	TestEqual(TEXT("ReplicationDistance"), static_cast<int32>(Message.ReplicationDistance), 3);
	TestEqual(TEXT("DecayRate"), static_cast<int32>(Message.DecayRate), 2);
	TestFalse(TEXT("bContainsAuth"), Message.bContainsAuth);
	TestEqual(TEXT("UUID"), Message.UUID, ActorFieldsGoldenActorId);
	TestEqual(TEXT("Timestamp"), Message.Timestamp, static_cast<int64>(987654321LL));
	TestEqual(TEXT("SequenceNumber"), static_cast<int32>(Message.SequenceNumber), 42);

	// GUID is USerializationFunctionLibrary::ToGuid(UUID), computed with the same production function
	// the body itself calls; this is not re-deriving the expectation independently, it is pinning that
	// the body still calls ToGuid on the UUID it just parsed.
	TestEqual(TEXT("GUID"), Message.GUID.ToString(),
		USerializationFunctionLibrary::ToGuid(ActorFieldsGoldenUuid).ToString());

	TestEqual(TEXT("StateSize"), Message.StateSize, StateSize);
	TestEqual(TEXT("StateBytes length"), Message.StateView.Num(), StateSize);
	TestTrue(TEXT("StateBytes content"), ActorFieldsBytesEqual(Message.StateView, State));
	TestEqual(TEXT("PayloadTypeID"), static_cast<int32>(Message.PayloadTypeID), static_cast<int32>(0xFFFF));
	TestFalse(TEXT("State stays invalid for an unregistered TypeID"), Message.State.IsValid());

	return true;
}

// One byte short of MetadataSize + TailSize: the frame cannot hold the envelope its opcode carries, so
// it is refused before the decoder is entered at all.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsActorNotificationMetadataTruncatedTest,
	FCrowdyMessageFieldsActorNoisyTest, "CrowdySDK.Wire.FieldsFActorUpdateNotificationMessage.MetadataTruncated",
	CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsActorNotificationMetadataTruncatedTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Data = BuildActorFieldsMetadata(1, 2, 3, 4, 0, 0, 0, ActorFieldsGoldenUuid);
	AppendLE(Data, static_cast<int32>(6));
	Data.Append(TArray<uint8>({ 0xFF, 0xFF, 0xAA, 0xBB, 0xCC, 0xDD }));
	AppendActorFieldsTail(Data, 1, 1);

	// MetadataSize + TailSize is the floor a spatial frame has to clear; one byte under it must reject
	// regardless of what a truncated StateSize/StateBytes region would otherwise have said.
	const int32 Floor = MetadataSize + TailSize;
	TArray<uint8> Truncated(Data.GetData(), Floor - 1);

	FActorUpdateNotificationMessage Message;
	TestFalse(TEXT("rejects a frame one byte short of the metadata+tail floor"),
		CrowdyWireParity::DecodeStrippedPayload(Message, Truncated));

	return true;
}

// StateSize declares more bytes than the frame has left after it. The check compares against bytes
// remaining rather than adding StateSize to the offset (the body's own comment explains why: adding
// first would overflow for a large declared length and wrap to a negative sum that passes), so pinning
// this needs a frame that is short by exactly one byte relative to the declared StateSize, not merely
// short overall.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsActorNotificationStateSizeTruncatedTest,
	"CrowdySDK.Wire.FieldsFActorUpdateNotificationMessage.StateSizeTruncated", CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsActorNotificationStateSizeTruncatedTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Data = BuildActorFieldsMetadata(1, 2, 3, 4, 0, 0, 0, ActorFieldsGoldenUuid);
	AppendLE(Data, static_cast<int32>(6)); // StateSize declares 6 bytes of state.

	// A body of nine octets: four for the StateSize field itself and five behind it, one short of the
	// six the frame claims to carry. Sized against the body rather than the whole message, since the
	// header and the trailer are taken off before the decoder sees anything.
	constexpr int32 BodyOctets = 9;
	while (Data.Num() < MetadataSize + BodyOctets + TailSize)
	{
		Data.Add(0);
	}
	check(Data.Num() == MetadataSize + BodyOctets + TailSize);

	FActorUpdateNotificationMessage Message;
	TestFalse(TEXT("rejects a declared StateSize that runs one byte past the end of the frame"),
		CrowdyWireParity::DecodeStrippedPayload(Message, Data));

	return true;
}

// StateSize <= 0 is rejected before the length-vs-frame check even runs; a well-formed frame otherwise.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsActorNotificationZeroStateSizeTest,
	"CrowdySDK.Wire.FieldsFActorUpdateNotificationMessage.ZeroStateSizeRejected", CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsActorNotificationZeroStateSizeTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Data = BuildActorFieldsMetadata(1, 2, 3, 4, 0, 0, 0, ActorFieldsGoldenUuid);
	AppendLE(Data, static_cast<int32>(0));
	AppendActorFieldsTail(Data, 1, 1);

	FActorUpdateNotificationMessage Message;
	TestFalse(TEXT("rejects a StateSize of zero"), CrowdyWireParity::DecodeStrippedPayload(Message, Data));

	return true;
}

namespace
{
	void AppendKitText(TArray<uint8>& Out, const ANSICHAR* Text)
	{
		const int32 Length = FCStringAnsi::Strlen(Text);
		check(Length < 256);
		if (Length < 32)
		{
			Out.Add(static_cast<uint8>(0xA0 | Length));
		}
		else
		{
			Out.Add(0xD9);
			Out.Add(static_cast<uint8>(Length));
		}
		Out.Append(reinterpret_cast<const uint8*>(Text), Length);
	}

	void AppendKitDouble(TArray<uint8>& Out, const double Value)
	{
		uint64 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		Out.Add(0xCB);
		for (int32 Shift = 56; Shift >= 0; Shift -= 8)
		{
			Out.Add(static_cast<uint8>(Bits >> Shift));
		}
	}

	// A Game Kit hub's mob as its realtime state carries it, written by hand as MessagePack: a map of kind, id, x, z, hp, max_hp
	// and mode, with no y, yaw, owner or name. A null Kind leaves out the kind, which makes it no kit actor at all.
	TArray<uint8> KitMobState(const ANSICHAR* Kind = "mob", const double X = 1.5)
	{
		TArray<uint8> Out;
		Out.Add(static_cast<uint8>(0x80 | (Kind ? 7 : 6)));
		if (Kind)
		{
			AppendKitText(Out, "kind");
			AppendKitText(Out, Kind);
		}
		AppendKitText(Out, "id");
		Out.Add(0x03);
		AppendKitText(Out, "x");
		AppendKitDouble(Out, X);
		AppendKitText(Out, "z");
		AppendKitDouble(Out, -2.0);
		AppendKitText(Out, "hp");
		Out.Add(0x28);
		AppendKitText(Out, "max_hp");
		Out.Add(0x64);
		AppendKitText(Out, "mode");
		AppendKitText(Out, "chase");
		return Out;
	}

	// A kit actor's id is its hub's text padded out, not hexadecimal.
	const FString KitMobUuid = TEXT("M10,-4@3________________________");
}

// A Game Kit hub's server-driven actor sends its state bare, as MessagePack with no length in front, so the SDK's framing does not
// fit it and the decoder reads it as a kit actor instead of refusing the frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsActorNotificationKitActorTest,
	"CrowdySDK.Wire.FieldsFActorUpdateNotificationMessage.KitActor", CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsActorNotificationKitActorTest::RunTest(const FString& Parameters)
{
	// A name already, as a starter's Kit Actors makes it.
	const FName Mob(TEXT("mob"));
	const TArray<uint8> State = KitMobState();
	TArray<uint8> Data = BuildActorFieldsMetadata(7, 1, 0, -2, 2, 0, 0, KitMobUuid);
	Data.Append(State);
	AppendActorFieldsTail(Data, 5000LL, 9);

	FActorUpdateNotificationMessage Message;
	if (!TestTrue(TEXT("accepts a kit actor's bare state"), CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
	{
		return false;
	}
	const FCrowdyKitActorState* Kit = Message.State.GetPtr<FCrowdyKitActorState>();
	if (!TestNotNull(TEXT("and decodes it as a kit actor"), Kit))
	{
		return false;
	}
	TestEqual(TEXT("its kind"), Kit->Kind, Mob);
	TestEqual(TEXT("its id in the hub"), Kit->Id, static_cast<int64>(3));
	TestEqual(TEXT("where it is, chunks turned into units"), Kit->Location, FVector(1.5, 0.0, -2.0) * FCrowdyKitActorState::ChunkWorldSize);
	TestTrue(TEXT("its health"), Kit->bHasHp && Kit->Hp == 40 && Kit->MaxHp == 100);
	TestEqual(TEXT("its mode"), Kit->Mode, FString(TEXT("chase")));
	TestTrue(TEXT("no owner, name or yaw it did not give"), Kit->Owner == 0 && Kit->Name.IsEmpty() && Kit->Rotation.IsZero());
	TestTrue(TEXT("the whole body is its state"), ActorFieldsBytesEqual(Message.StateView, State));
	TestEqual(TEXT("it names no SDK type"), static_cast<int32>(Message.PayloadTypeID), static_cast<int32>(CROWDY_INVALID_TYPE_ID));
	TestEqual(TEXT("its key is its id's"), Message.GUID, USerializationFunctionLibrary::ToGuid(KitMobUuid));
	TestEqual(TEXT("its hub's Instance Id is its id's"), Kit->InstanceId, FString(TEXT("10,-4")));

	const auto Frame = [](const TArray<uint8>& Body)
	{
		TArray<uint8> Out = BuildActorFieldsMetadata(7, 1, 0, -2, 2, 0, 0, KitMobUuid);
		Out.Append(Body);
		AppendActorFieldsTail(Out, 5000LL, 9);
		return Out;
	};
	FActorUpdateNotificationMessage Refused;
	TestFalse(TEXT("a map with no kind is no kit actor, and the frame is refused as before"), CrowdyWireParity::DecodeStrippedPayload(Refused, Frame(KitMobState(nullptr))));
	TestFalse(TEXT("nor is one with a kind longer than any starter names"),
		CrowdyWireParity::DecodeStrippedPayload(Refused, Frame(KitMobState("kind_padded_out_well_past_the_sixty_four_bytes_a_kind_may_take_up"))));
	TestFalse(TEXT("nor one with a place that is not a number"), CrowdyWireParity::DecodeStrippedPayload(Refused, Frame(KitMobState("mob", std::numeric_limits<double>::quiet_NaN()))));
	TestFalse(TEXT("nor one past the edge of any world"), CrowdyWireParity::DecodeStrippedPayload(Refused, Frame(KitMobState("mob", 1.0e300))));

	// Its first four bytes read as a negative length rather than one too long.
	TArray<uint8> XFirst;
	XFirst.Add(0x84);
	AppendKitText(XFirst, "x");
	AppendKitDouble(XFirst, 1.5);
	AppendKitText(XFirst, "kind");
	AppendKitText(XFirst, "mob");
	AppendKitText(XFirst, "id");
	XFirst.Add(0x03);
	AppendKitText(XFirst, "z");
	AppendKitDouble(XFirst, -2.0);
	FActorUpdateNotificationMessage Reordered;
	TestTrue(TEXT("a state with another key first is a kit actor too"), CrowdyWireParity::DecodeStrippedPayload(Reordered, Frame(XFirst))
		&& Reordered.State.GetPtr<FCrowdyKitActorState>() != nullptr);

	FActorUpdateNotificationMessage Unnamed;
	if (TestTrue(TEXT("a kind no starter names still decodes"),
		CrowdyWireParity::DecodeStrippedPayload(Unnamed, Frame(KitMobState("kind_no_starter_has_named_3f1c")))))
	{
		TestTrue(TEXT("but adds no name, as None"), Unnamed.State.GetPtr<FCrowdyKitActorState>()->Kind.IsNone()
			&& FName(TEXT("kind_no_starter_has_named_3f1c"), FNAME_Find).IsNone());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsActorNotificationKitInstanceIdTest,
	"CrowdySDK.Wire.FieldsFActorUpdateNotificationMessage.KitActorInstanceId", CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsActorNotificationKitInstanceIdTest::RunTest(const FString& Parameters)
{
	const auto Read = [](const ANSICHAR* Text)
	{
		uint8 Octets[FCrowdyActorId::NumOctets] = {};
		FMemory::Memcpy(Octets, Text, FMath::Min<int32>(FCStringAnsi::Strlen(Text), FCrowdyActorId::NumOctets));
		return FCrowdyKitActorState::ReadInstanceId(MakeArrayView(Octets, FCrowdyActorId::NumOctets));
	};
	TestEqual(TEXT("a zero-padded npcs id names its area"), Read("P-2,13@41"), FString(TEXT("-2,13")));
	TestEqual(TEXT("the first @ ends it"), Read("N0,0@7@9"), FString(TEXT("0,0")));
	TestEqual(TEXT("a ball's id names its match"), Read("Bball-bp-x@1"), FString(TEXT("ball-bp-x")));
	TestEqual(TEXT("an id filling all 32 octets"), Read("M-4294967296,-4294967296@9999999"), FString(TEXT("-4294967296,-4294967296")));
	TestEqual(TEXT("an earlier npcs id, which named its centre chunk, names no hub rather than the wrong one"), Read("M42,-14:3"), FString());
	TestEqual(TEXT("a lower-case start names no hub"), Read("ball@abc"), FString());
	TestEqual(TEXT("nor an empty Instance Id"), Read("M@3"), FString());
	TestEqual(TEXT("nor an id with no @"), Read("M2,-1"), FString());
	TestEqual(TEXT("nor one with a space before the @"), Read("M2, 1@3"), FString());
	TestEqual(TEXT("nor a hexadecimal player id"), Read("0123456789abcdef0123456789abcdef"), FString());
	return true;
}

// FActorUpdateRequestMessage::DecodePayload has no accept path at all today: it logs a warning and
// unconditionally returns false, regardless of what Data holds. That makes it the one type in this file
// without a positive-acceptance vector; both vectors below pin the reject-everything behavior instead,
// one on empty input and one on bytes shaped like a real frame, to show the unconditional return does
// not depend on what was handed to it. The parser itself never reaches this type (opcode
// ACTOR_UPDATE_REQUEST has no case in FCrowdyMessageParser::ParseMessage's switch), so this exercises
// the body directly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsActorRequestAlwaysRejectsTest,
	"CrowdySDK.Wire.FieldsFActorUpdateRequestMessage.AlwaysRejects", CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsActorRequestAlwaysRejectsTest::RunTest(const FString& Parameters)
{
	FActorUpdateRequestMessage EmptyMessage;
	TestFalse(TEXT("rejects empty input"),
		CrowdyWireParity::DecodeStrippedPayload(EmptyMessage, TArray<uint8>()));

	TArray<uint8> WellFormed = BuildActorFieldsMetadata(1, 2, 3, 4, 0, 0, 0, ActorFieldsGoldenUuid);
	AppendLE(WellFormed, static_cast<int32>(4));
	WellFormed.Append(TArray<uint8>({ 1, 2, 3, 4 }));
	AppendActorFieldsTail(WellFormed, 1, 1);

	FActorUpdateRequestMessage WellFormedMessage;
	TestFalse(TEXT("rejects a frame shaped like a real actor update notification"),
		CrowdyWireParity::DecodeStrippedPayload(WellFormedMessage, WellFormed));

	return true;
}

// FSingleActorNotification inherits FGameEventNotification::DecodePayload unmodified; only the opcode
// GetType() returns differs, and DecodePayload never looks at that. This decodes an unregistered event
// TypeID (0xFFFF), which DeserializeEventState reports as a rate-gated warning rather than an error, so
// nothing here has to whitelist it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsSingleActorNotificationAcceptNoEnvelopeTest,
	"CrowdySDK.Wire.FieldsFSingleActorNotification.AcceptNoEnvelope", CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsSingleActorNotificationAcceptNoEnvelopeTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> State = { 0xFF, 0xFF }; // uint16 TypeID 0xFFFF, deliberately unregistered.
	const int32 StateSize = State.Num();

	TArray<uint8> Data = BuildActorFieldsMetadata(42, 1, -1, 2, 4, 1, 0, ActorFieldsGoldenUuid);
	AppendLE(Data, static_cast<uint16>(0x1234)); // EventType
	AppendLE(Data, StateSize);
	Data.Append(State);
	// No target block. The body a decoder is handed already has the header and the trailer taken off it,
	// and this one ends with the state, so nothing at all is left behind the state. That is fewer than the
	// 33 octets a target byte plus a 32 octet target id would need, so Target and TargetID keep their
	// broadcast defaults instead of being read off whatever happens to follow.
	AppendActorFieldsTail(Data, 555, 3);

	FSingleActorNotification Message;
	if (!TestTrue(TEXT("accepts a well-formed frame with no envelope"),
		CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
	{
		return false;
	}

	TestEqual(TEXT("AppID"), Message.AppID, static_cast<int64>(42));
	TestEqual(TEXT("ChunkX"), Message.ChunkX, static_cast<int64>(1));
	TestEqual(TEXT("ChunkY"), Message.ChunkY, static_cast<int64>(-1));
	TestEqual(TEXT("ChunkZ"), Message.ChunkZ, static_cast<int64>(2));
	TestEqual(TEXT("ReplicationDistance"), static_cast<int32>(Message.ReplicationDistance), 4);
	TestEqual(TEXT("DecayRate"), static_cast<int32>(Message.DecayRate), 1);
	TestFalse(TEXT("bContainsAuth"), Message.bContainsAuth);
	TestEqual(TEXT("UUID"), Message.UUID, ActorFieldsGoldenActorId);
	TestEqual(TEXT("Timestamp"), Message.Timestamp, static_cast<int64>(555));
	TestEqual(TEXT("SequenceNumber"), static_cast<int32>(Message.SequenceNumber), 3);

	TestEqual(TEXT("EventType"), static_cast<int32>(Message.EventType), 0x1234);
	TestEqual(TEXT("StateSize"), Message.StateSize, StateSize);
	TestEqual(TEXT("StateBytes length"), Message.StateView.Num(), StateSize);
	TestTrue(TEXT("StateBytes content"), ActorFieldsBytesEqual(Message.StateView, State));
	TestFalse(TEXT("State stays invalid for an unregistered TypeID"), Message.State.IsValid());

	// No trailing envelope bytes were supplied, so Target/TargetID stay at their pre-envelope defaults.
	TestEqual(TEXT("Target defaults to Everyone"), static_cast<int32>(Message.Target),
		static_cast<int32>(ECrowdyTarget::Everyone));
	TestFalse(TEXT("TargetID stays unset"), Message.TargetID.IsValid());

	return true;
}

// Same layout, with the optional Target byte and 32-hex-char TargetID appended between the state and the
// tail. The body a decoder is handed has the header and the trailer off it already, so the octets left
// behind the state are exactly the 33 that block occupies: the smallest remainder for which it is read at
// all, and one octet fewer would leave it unread.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsSingleActorNotificationAcceptWithEnvelopeTest,
	"CrowdySDK.Wire.FieldsFSingleActorNotification.AcceptWithEnvelope", CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsSingleActorNotificationAcceptWithEnvelopeTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> State = { 0xFF, 0xFF };
	const int32 StateSize = State.Num();

	TArray<uint8> Data = BuildActorFieldsMetadata(7, 0, 0, 0, 0, 0, 0, ActorFieldsGoldenUuid);
	AppendLE(Data, static_cast<uint16>(0x5678)); // EventType
	AppendLE(Data, StateSize);
	Data.Append(State);

	// Envelope: Target = Entity(1), then the 32-hex-char TargetID.
	Data.Add(static_cast<uint8>(ECrowdyTarget::Entity));
	const FTCHARToUTF8 ConvertedTargetId(*ActorFieldsTargetIdHex);
	check(ConvertedTargetId.Length() == 32);
	Data.Append(reinterpret_cast<const uint8*>(ConvertedTargetId.Get()), 32);

	AppendActorFieldsTail(Data, 1, 1);

	FSingleActorNotification Message;
	if (!TestTrue(TEXT("accepts a well-formed frame with an envelope"),
		CrowdyWireParity::DecodeStrippedPayload(Message, Data)))
	{
		return false;
	}

	TestEqual(TEXT("EventType"), static_cast<int32>(Message.EventType), 0x5678);
	TestEqual(TEXT("StateSize"), Message.StateSize, StateSize);
	TestTrue(TEXT("StateBytes content"), ActorFieldsBytesEqual(Message.StateView, State));

	TestEqual(TEXT("Target"), static_cast<int32>(Message.Target), static_cast<int32>(ECrowdyTarget::Entity));
	TestEqual(TEXT("TargetID"), Message.TargetID.ToString(),
		USerializationFunctionLibrary::ToGuid(ActorFieldsTargetIdHex).ToString());

	return true;
}

// One byte short of MetadataSize + TailSize, same floor as the actor notification's metadata check.
IMPLEMENT_CUSTOM_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsSingleActorNotificationMetadataTruncatedTest,
	FCrowdyMessageFieldsActorNoisyTest, "CrowdySDK.Wire.FieldsFSingleActorNotification.MetadataTruncated",
	CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsSingleActorNotificationMetadataTruncatedTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Data = BuildActorFieldsMetadata(1, 2, 3, 4, 0, 0, 0, ActorFieldsGoldenUuid);
	AppendLE(Data, static_cast<uint16>(1));
	AppendLE(Data, static_cast<int32>(2));
	Data.Append(TArray<uint8>({ 0xFF, 0xFF }));
	AppendActorFieldsTail(Data, 1, 1);

	const int32 Floor = MetadataSize + TailSize;
	TArray<uint8> Truncated(Data.GetData(), Floor - 1);

	FSingleActorNotification Message;
	TestFalse(TEXT("rejects a frame one byte short of the metadata+tail floor"),
		CrowdyWireParity::DecodeStrippedPayload(Message, Truncated));

	return true;
}

// StateSize declares more than the frame has left after it. Unlike the actor notification, this check
// (Offset + StateSize > Data.Num()) is a plain forward comparison rather than a bytes-remaining one, but
// it still needs to be tested exactly at its boundary: one byte short of the declared length.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMessageFieldsSingleActorNotificationStateSizeTruncatedTest,
	"CrowdySDK.Wire.FieldsFSingleActorNotification.StateSizeTruncated", CrowdyMessageFieldsActorTestFlags)
bool FCrowdyMessageFieldsSingleActorNotificationStateSizeTruncatedTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Data = BuildActorFieldsMetadata(1, 2, 3, 4, 0, 0, 0, ActorFieldsGoldenUuid);
	AppendLE(Data, static_cast<uint16>(1)); // EventType
	AppendLE(Data, static_cast<int32>(10)); // StateSize declares 10 bytes of state.

	// A body of EventType(2) + StateSize(4) + nine state octets, one short of the ten declared. Sized
	// against the body, since the header and the trailer come off before the decoder reads anything, and
	// the whole point of this vector is to sit exactly on the state-length check rather than merely
	// somewhere past it.
	constexpr int32 BodyOctets = 2 + 4 + 9;
	while (Data.Num() < MetadataSize + BodyOctets + TailSize)
	{
		Data.Add(0);
	}
	check(Data.Num() == MetadataSize + BodyOctets + TailSize);

	FSingleActorNotification Message;
	TestFalse(TEXT("rejects a declared StateSize that runs one byte past the end of the frame"),
		CrowdyWireParity::DecodeStrippedPayload(Message, Data));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
