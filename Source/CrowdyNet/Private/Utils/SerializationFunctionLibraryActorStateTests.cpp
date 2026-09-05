#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "CrowdyNetActorStateWireTestTypes.h"
#include "CrowdyNetLog.h"
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "StructUtils/InstancedStruct.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UActorUpdatePayloadRegistry.h"

// Covers DeserializeActorState's own framing decisions directly: a payload shorter than the type tag,
// a payload that resolves a real type then runs out of bytes partway through the struct it names, and a
// well-formed payload. Every input byte array is authored literally (built field by field with AppendLE,
// never produced by calling SerializeActorState), so a bug shared between the writer and the reader
// cannot hide from these the way it would in a round trip that only compares a decode against its own
// encode.
namespace
{
	constexpr EAutomationTestFlags CrowdyNetActorStateTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A payload this short (or this truncated) makes DeserializeActorState log a Warning, not an Error
	// (see SerializationFunctionLibrary.cpp: a malformed frame is an ordinary event on an open network,
	// not a fault in this build), so nothing here needs AddExpectedError.
	template <typename T>
	void AppendLE(TArray<uint8>& Out, T Value)
	{
		const int32 Base = Out.Num();
		Out.SetNumUninitialized(Base + sizeof(T));
		FMemory::Memcpy(Out.GetData() + Base, &Value, sizeof(T));
	}

	// Registration is safe to repeat (UActorUpdatePayloadRegistry::RegisterStruct silently ignores a
	// duplicate registration of the same struct under the same id), so every test in this file can call
	// this at the top without caring whether an earlier test already did.
	constexpr FCrowdyTypeID CrowdyNetActorStateWireTestTypeID = 61234;

	void RegisterActorStateWireTestPayload()
	{
		UActorUpdatePayloadRegistry::Get()->RegisterStruct(
			FCrowdyNetActorStateWireTestPayload::StaticStruct(), CrowdyNetActorStateWireTestTypeID);
	}
}

// Shorter than sizeof(FCrowdyTypeID): there is no tag to read at all, so this has to be refused before
// the registry is even consulted. A payload naming no type is the one thing this decoder does reject
// outright, which is what makes it worth pinning separately from the short-payload cases below.
//
// The out payload is pre-seeded with a previous frame's decode on purpose. Every rejection path empties
// it, so a caller that reuses one payload across calls can never be handed back the frame before this
// one; asserting only that a fresh payload stays empty would pass even if that were untrue.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySerializationDeserializeActorStateOneByteTest,
	"CrowdySDK.Wire.DeserializeActorStateOneByte", CrowdyNetActorStateTestFlags)
bool FCrowdySerializationDeserializeActorStateOneByteTest::RunTest(const FString& Parameters)
{
	RegisterActorStateWireTestPayload();

	const TArray<uint8> Payload = {0xAB};

	FInstancedStruct OutPayload;
	OutPayload.InitializeAs(FCrowdyNetActorStateWireTestPayload::StaticStruct());

	TestFalse(TEXT("a payload shorter than the type tag is refused"),
		USerializationFunctionLibrary::DeserializeActorState(Payload, OutPayload));
	TestFalse(TEXT("the previous frame the caller was holding is cleared rather than handed back"),
		OutPayload.IsValid());

	return true;
}

// The tag resolves to a real, registered struct, but the payload runs out of bytes one field into it.
// SerializeBin cannot tell that apart from "an older sender never wrote this field at all":
// FMemoryReader::Serialize sets the archive's error flag the instant a read would run past the payload's
// own length, and nothing in the framing says whether the payload is short because it is corrupt or
// short because it predates a field this build added. Since the two are indistinguishable, the decoder
// picks the reading that keeps an older peer working and leaves the unreached fields at the values the
// struct constructs them with. That is what this test pins.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySerializationDeserializeActorStateResolvesThenTruncatesTest,
	"CrowdySDK.Wire.DeserializeActorStateResolvesThenTruncates", CrowdyNetActorStateTestFlags)
bool FCrowdySerializationDeserializeActorStateResolvesThenTruncatesTest::RunTest(const FString& Parameters)
{
	RegisterActorStateWireTestPayload();

	TArray<uint8> Payload;
	AppendActorStateFraming(Payload, CrowdyNetActorStateWireTestTypeID, CrowdyActorStateWireTestClassID);
	AppendLE(Payload, static_cast<int32>(111)); // Field A only; field B (4 more bytes) is missing.

	// Accepted, with field B left at its default. A state struct only ever grows by appending fields
	// and the type tag does not change when it does, so a payload shorter than this build's idea of
	// the struct is what a peer one version behind legitimately sends. Nothing in the framing can tell
	// that apart from a frame cut off mid-struct, so the choice is which way to be wrong, and dropping
	// every update from an older peer is the worse way.
	FInstancedStruct OutPayload;
	TestTrue(TEXT("a payload that resolves a type then runs out of bytes still decodes"),
		USerializationFunctionLibrary::DeserializeActorState(Payload, OutPayload));

	const FCrowdyNetActorStateWireTestPayload* Decoded = OutPayload.GetPtr<FCrowdyNetActorStateWireTestPayload>();
	if (!TestNotNull(TEXT("decoded payload holds the registered struct type"), Decoded))
	{
		return false;
	}

	TestEqual(TEXT("the field the payload did carry is honoured"), Decoded->A, 111);
	TestEqual(TEXT("the field past the end of the payload keeps its constructed default"), Decoded->B,
		FCrowdyNetActorStateWireTestPayload().B);

	return true;
}

// A fully-formed frame: tag, then both fields, with nothing missing and nothing left over. This is the
// control the two rejection tests above need: 3.1's hardening must not have started rejecting input that
// was never malformed to begin with.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySerializationDeserializeActorStateWellFormedTest,
	"CrowdySDK.Wire.DeserializeActorStateWellFormed", CrowdyNetActorStateTestFlags)
bool FCrowdySerializationDeserializeActorStateWellFormedTest::RunTest(const FString& Parameters)
{
	RegisterActorStateWireTestPayload();

	TArray<uint8> Payload;
	AppendActorStateFraming(Payload, CrowdyNetActorStateWireTestTypeID, CrowdyActorStateWireTestClassID);
	AppendLE(Payload, static_cast<int32>(-42));
	AppendLE(Payload, static_cast<int32>(777));

	FInstancedStruct OutPayload;
	FCrowdyTypeID OutTypeID = CROWDY_INVALID_TYPE_ID;
	FCrowdyClassID OutClassID = CROWDY_INVALID_CLASS_ID;
	TestTrue(TEXT("a well-formed payload still decodes"),
		USerializationFunctionLibrary::DeserializeActorState(Payload, OutPayload, OutTypeID, OutClassID));

	TestEqual(TEXT("the type tag reported back matches what was registered"),
		static_cast<int32>(OutTypeID), static_cast<int32>(CrowdyNetActorStateWireTestTypeID));

	// The whole point of the framing: shape and identity are two separate facts, and the class comes off
	// the message rather than being inferred from the struct that carried it.
	TestEqual(TEXT("the class id reported back is the one the frame carried"),
		static_cast<int64>(OutClassID), static_cast<int64>(CrowdyActorStateWireTestClassID));

	const FCrowdyNetActorStateWireTestPayload* Decoded = OutPayload.GetPtr<FCrowdyNetActorStateWireTestPayload>();
	if (!TestNotNull(TEXT("decoded payload holds the registered struct type"), Decoded))
	{
		return false;
	}

	// Compared against the literal values authored above, not against a second call to
	// SerializeActorState: a bug that drops or misorders a field on the write side would be invisible to
	// a round trip that only checks the two sides agree with each other.
	TestEqual(TEXT("field A matches the authored literal"), Decoded->A, -42);
	TestEqual(TEXT("field B matches the authored literal"), Decoded->B, 777);

	return true;
}

// A byte-for-byte frame in the format that shipped BEFORE class identity went on the wire: the type tag
// leads, with no sentinel, no version and no class id. It must be REFUSED.
//
// This is the test the whole framing turns on, and the reason the sentinel is a reserved uint16 rather
// than the leading version byte an earlier draft called for. Under that draft this frame's first byte
// would have been read as a format version, and for any struct whose type tag happens to have that
// value in its low byte, EVERY legacy frame would have been accepted and four bytes of its body read as
// a class id. The tag is a hash fixed per struct, so that is a permanent property of the struct rather
// than a one-in-256 risk. GenerateFromStruct maps every struct into 1..65535, so no legacy frame can
// begin with the sentinel and the two formats are distinguishable with certainty.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySerializationDeserializeActorStateLegacyFrameRefusedTest,
	"CrowdySDK.Wire.DeserializeActorStateLegacyFrameRefused", CrowdyNetActorStateTestFlags)
bool FCrowdySerializationDeserializeActorStateLegacyFrameRefusedTest::RunTest(const FString& Parameters)
{
	RegisterActorStateWireTestPayload();

	// Exactly what the previous build's SerializeActorState emitted: [uint16 TypeID][body].
	TArray<uint8> LegacyPayload;
	AppendLE(LegacyPayload, CrowdyNetActorStateWireTestTypeID);
	AppendLE(LegacyPayload, static_cast<int32>(-42));
	AppendLE(LegacyPayload, static_cast<int32>(777));

	FInstancedStruct OutPayload;
	OutPayload.InitializeAs(FCrowdyNetActorStateWireTestPayload::StaticStruct());

	TestFalse(TEXT("a frame in the pre-class-identity format is refused"),
		USerializationFunctionLibrary::DeserializeActorState(LegacyPayload, OutPayload));
	TestFalse(TEXT("and the frame the caller was holding is cleared rather than handed back"),
		OutPayload.IsValid());

	return true;
}

// A frame that IS framed, and declares a format version this build does not decode. Refused before the
// body is touched.
//
// This is the one rejection on skew the actor-state path is allowed to make. Everything downstream of it
// tolerates a peer a version apart on purpose, because a payload struct grows by appending and a shorter
// frame from an older peer is still valid; that tolerance is structurally unable to tell an appended
// field from a reshaped frame, so a reshaping has to be caught here or it is never caught at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySerializationDeserializeActorStateUnknownVersionRefusedTest,
	"CrowdySDK.Wire.DeserializeActorStateUnknownVersionRefused", CrowdyNetActorStateTestFlags)
bool FCrowdySerializationDeserializeActorStateUnknownVersionRefusedTest::RunTest(const FString& Parameters)
{
	RegisterActorStateWireTestPayload();

	TArray<uint8> Payload;
	const FCrowdyTypeID Sentinel = CROWDY_ACTOR_STATE_SENTINEL;
	const uint8 FutureVersion = static_cast<uint8>(CROWDY_ACTOR_STATE_FORMAT_VERSION + 1);

	AppendLE(Payload, Sentinel);
	Payload.Add(FutureVersion);
	AppendLE(Payload, CrowdyNetActorStateWireTestTypeID);
	AppendLE(Payload, CrowdyActorStateWireTestClassID);
	AppendLE(Payload, static_cast<int32>(-42));
	AppendLE(Payload, static_cast<int32>(777));

	FInstancedStruct OutPayload;
	OutPayload.InitializeAs(FCrowdyNetActorStateWireTestPayload::StaticStruct());

	TestFalse(TEXT("a frame declaring an unknown format version is refused"),
		USerializationFunctionLibrary::DeserializeActorState(Payload, OutPayload));
	TestFalse(TEXT("and the frame the caller was holding is cleared rather than handed back"),
		OutPayload.IsValid());

	return true;
}

// A frame that names no class at all. A conforming sender never emits this, because SerializeActorState
// refuses to, so seeing it means a forged or corrupt frame.
//
// Refusing it is what keeps the guarantee the framing is built on: an accepted update always names its
// own class, so nothing downstream ever has to fall back to deriving one from the wire struct. Falling
// back is not a harmless default, it is the impersonation defect this whole change exists to delete,
// where two classes sharing one struct collapse into whichever registered last.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySerializationDeserializeActorStateNoClassIdRefusedTest,
	"CrowdySDK.Wire.DeserializeActorStateNoClassIdRefused", CrowdyNetActorStateTestFlags)
bool FCrowdySerializationDeserializeActorStateNoClassIdRefusedTest::RunTest(const FString& Parameters)
{
	RegisterActorStateWireTestPayload();

	TArray<uint8> Payload;
	AppendActorStateFraming(Payload, CrowdyNetActorStateWireTestTypeID, CROWDY_INVALID_CLASS_ID);
	AppendLE(Payload, static_cast<int32>(-42));
	AppendLE(Payload, static_cast<int32>(777));

	FInstancedStruct OutPayload;
	OutPayload.InitializeAs(FCrowdyNetActorStateWireTestPayload::StaticStruct());

	TestFalse(TEXT("a frame carrying no class id is refused"),
		USerializationFunctionLibrary::DeserializeActorState(Payload, OutPayload));
	TestFalse(TEXT("and the frame the caller was holding is cleared rather than handed back"),
		OutPayload.IsValid());

	return true;
}

// THE case this whole framing exists for, and the one that was structurally impossible before it: two
// DIFFERENT entity classes sending the SAME wire struct, told apart by the receiver.
//
// Before, a receiver derived the class from the state struct through a struct-to-class map that
// `RegisterStateClass` populated with `Add`, so the last registrant won and both classes resolved as
// one. No better guess could have fixed that, because the information simply was not on the wire. It
// was observed rather than theorised: crowd-sim bots sending FTitanAssaultActorState resolved on an
// observer as BP_TA_PlayerCharacter_C while their actual class was ACKCrowdSimStateEntity, and 376 of
// 427 deltas were refused by a class guard that was right about a mismatch it should never have seen.
//
// Pinned here at the wire level, where it is decidable without a world: one shape, two identities,
// carried and recovered independently. That the recovered ids then resolve to two live UClasses on a
// real observer is the two-client gate's job, and it stays open.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySerializationTwoClassesShareOneStateStructTest,
	"CrowdySDK.Wire.TwoClassesShareOneStateStruct", CrowdyNetActorStateTestFlags)
bool FCrowdySerializationTwoClassesShareOneStateStructTest::RunTest(const FString& Parameters)
{
	RegisterActorStateWireTestPayload();

	// Two ids that differ, derived the way the registry derives them, so this is not asserting against
	// two numbers invented for the test.
	const FCrowdyClassID FirstClassID = FCrowdyTypeIDGenerator::GenerateFromClass(AActor::StaticClass());
	const FCrowdyClassID SecondClassID = FCrowdyTypeIDGenerator::GenerateFromClass(APawn::StaticClass());

	if (!TestNotEqual(TEXT("the two classes derive different wire ids"),
		static_cast<int64>(FirstClassID), static_cast<int64>(SecondClassID)))
	{
		return false;
	}

	// Same TypeID in both frames. That is the point: one shape, two senders, nothing about the bytes of
	// the body distinguishing them.
	TArray<uint8> FirstPayload;
	AppendActorStateFraming(FirstPayload, CrowdyNetActorStateWireTestTypeID, FirstClassID);
	AppendLE(FirstPayload, static_cast<int32>(1));
	AppendLE(FirstPayload, static_cast<int32>(2));

	TArray<uint8> SecondPayload;
	AppendActorStateFraming(SecondPayload, CrowdyNetActorStateWireTestTypeID, SecondClassID);
	AppendLE(SecondPayload, static_cast<int32>(1));
	AppendLE(SecondPayload, static_cast<int32>(2));

	FInstancedStruct FirstDecoded;
	FCrowdyTypeID FirstTypeID = CROWDY_INVALID_TYPE_ID;
	FCrowdyClassID FirstDecodedClassID = CROWDY_INVALID_CLASS_ID;
	TestTrue(TEXT("the first frame decodes"),
		USerializationFunctionLibrary::DeserializeActorState(FirstPayload, FirstDecoded, FirstTypeID, FirstDecodedClassID));

	FInstancedStruct SecondDecoded;
	FCrowdyTypeID SecondTypeID = CROWDY_INVALID_TYPE_ID;
	FCrowdyClassID SecondDecodedClassID = CROWDY_INVALID_CLASS_ID;
	TestTrue(TEXT("the second frame decodes"),
		USerializationFunctionLibrary::DeserializeActorState(SecondPayload, SecondDecoded, SecondTypeID, SecondDecodedClassID));

	// Shape: identical, and both resolve to the same registered struct.
	TestEqual(TEXT("both frames declare the same wire struct"),
		static_cast<int32>(FirstTypeID), static_cast<int32>(SecondTypeID));
	TestNotNull(TEXT("the first frame decodes as the registered struct"),
		FirstDecoded.GetPtr<FCrowdyNetActorStateWireTestPayload>());
	TestNotNull(TEXT("the second frame decodes as the registered struct"),
		SecondDecoded.GetPtr<FCrowdyNetActorStateWireTestPayload>());

	// Identity: recovered separately. Asserted against the authored ids rather than only against each
	// other, so a decoder that returned two different but wrong values could not pass.
	TestEqual(TEXT("the first frame's class survives the round trip"),
		static_cast<int64>(FirstDecodedClassID), static_cast<int64>(FirstClassID));
	TestEqual(TEXT("the second frame's class survives the round trip"),
		static_cast<int64>(SecondDecodedClassID), static_cast<int64>(SecondClassID));
	TestNotEqual(TEXT("and the two are told apart despite sharing a wire struct"),
		static_cast<int64>(FirstDecodedClassID), static_cast<int64>(SecondDecodedClassID));

	return true;
}

// The sender's half of the same rule: a payload whose class cannot be named is refused at the writer
// rather than put on the wire for a receiver to reject. Pinned separately because the two refusals are
// independent code paths, and a sender that emitted an unnameable frame would be a silent per-update
// drop on every observer rather than one error where the cause is.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySerializationSerializeActorStateRefusesNoClassIdTest,
	"CrowdySDK.Wire.SerializeActorStateRefusesNoClassId", CrowdyNetActorStateTestFlags)
bool FCrowdySerializationSerializeActorStateRefusesNoClassIdTest::RunTest(const FString& Parameters)
{
	RegisterActorStateWireTestPayload();

	AddExpectedErrorPlain(TEXT("Refusing to send"), EAutomationExpectedErrorFlags::Contains, 0);

	FCrowdyNetActorStateWireTestPayload Payload;
	Payload.A = 5;

	TArray<uint8> OutBytes;
	TestFalse(TEXT("a payload with no class id is refused by the writer"),
		USerializationFunctionLibrary::SerializeActorState(
			FInstancedStruct::Make(Payload), CROWDY_INVALID_CLASS_ID, OutBytes));
	TestEqual(TEXT("and nothing was written"), OutBytes.Num(), 0);

	return true;
}

// The fine-grained decode scope is nested inside a scope that is itself measured, so leaving it on costs
// two timestamps per received message and makes a before-and-after reading of the enclosing scope
// compare two different instrumentations. Its default is the only thing that keeps those readings
// comparable, and a default is exactly the kind of value a debugging session flips and forgets.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySerializationDecodeScopesAreOffByDefaultTest,
	"CrowdySDK.Wire.DecodeScopesAreOffByDefault", CrowdyNetActorStateTestFlags)
bool FCrowdySerializationDecodeScopesAreOffByDefaultTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* const Cvar = IConsoleManager::Get().FindConsoleVariable(TEXT("crowdy.serialize.scopes"));

	if (!TestNotNull(TEXT("crowdy.serialize.scopes exists"), Cvar))
	{
		return false;
	}

	// Nothing in this suite sets it, so what it reads here is the value it was registered with. Asserted
	// through the decode path's own accessor as well, so a gate reading a different variable than the one
	// checked above cannot pass.
	TestEqual(TEXT("it reads zero in a session that never set it"), Cvar->GetInt(), 0);
	TestFalse(TEXT("and the decode path sees it off"), CrowdyNetProfile::DecodeScopes());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
