#if WITH_DEV_AUTOMATION_TESTS

#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Core/UDP/CrowdyKitEvents.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Messages/GameObjects/FKitEventRequest.h"
#include "Messages/GameObjects/FSingleActorMessage.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Network/UDP/CrowdyCppSendAdapter.h"
#include "Utils/UEventPayloadRegistry.h"
#include "CrowdyWireParitySupport.h"

namespace
{
	constexpr EAutomationTestFlags KitEventWireTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The bytes CrowdyCPP's encodeEmoteBody writes: keys in sorted order, no spaces, targetUuid always present.
	const ANSICHAR* const KitEventWireJsonNoTarget = "{\"emote\":\"wave\",\"targetUuid\":null}";
	const ANSICHAR* const KitEventWireJsonWithTarget =
		"{\"emote\":\"wave\",\"targetUuid\":\"fedcba9876543210fedcba9876543210\"}";
	const ANSICHAR* const KitEventWireSender = "0123456789abcdef0123456789abcdef";

	TArray<uint8> KitEventWireAscii(const ANSICHAR* Text)
	{
		TArray<uint8> Out;
		for (const ANSICHAR* Cursor = Text; *Cursor; ++Cursor)
		{
			Out.Add(static_cast<uint8>(*Cursor));
		}
		return Out;
	}

	void KitEventWireAppendLittleEndian64(TArray<uint8>& Out, const int64 Value)
	{
		const uint64 Bits = static_cast<uint64>(Value);
		for (int32 Shift = 0; Shift < 64; Shift += 8)
		{
			Out.Add(static_cast<uint8>((Bits >> Shift) & 0xff));
		}
	}

	// A frame as a decoder is handed it: the spatial header with no opcode, the payload, then the nine octet tail.
	TArray<uint8> KitEventWireStrippedFrame(const TArray<uint8>& Payload)
	{
		TArray<uint8> Data;
		KitEventWireAppendLittleEndian64(Data, 7);
		KitEventWireAppendLittleEndian64(Data, 1);
		KitEventWireAppendLittleEndian64(Data, -2);
		KitEventWireAppendLittleEndian64(Data, 3);
		Data.Add(2);
		Data.Add(0);
		Data.Add(0);
		Data.Append(KitEventWireAscii(KitEventWireSender));
		Data.Append(Payload);
		KitEventWireAppendLittleEndian64(Data, 1234);
		Data.Add(5);
		return Data;
	}

	// What CrowdyCPP's sendClientEvent puts on the wire for an event: its own encoder, not ours.
	TArray<uint8> KitEventWireCrowdyCppPayload(const uint16 EventType, const TArray<uint8>& State)
	{
		TArray<uint8> Payload;
		Payload.SetNumUninitialized(State.Num() + 2);
		const crowdy::Result<std::size_t> Written = crowdy::wire::encodeEventPayload(EventType,
			crowdy::Bytes(State.GetData(), static_cast<std::size_t>(State.Num())),
			crowdy::MutableBytes(Payload.GetData(), static_cast<std::size_t>(Payload.Num())));
		check(Written.ok());
		return Payload;
	}

	FString KitEventWireText(TConstArrayView<uint8> Bytes)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
	}

	class FKitEventWireWarningCapture : public FOutputDevice
	{
	public:
		virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
			if (Level <= ELogVerbosity::Warning && Category == FName(TEXT("LogCrowdyNet")))
			{
				FScopeLock Lock(&Mutex);
				++Count;
			}
		}

		int32 Read()
		{
			FScopeLock Lock(&Mutex);
			return Count;
		}

	private:
		FCriticalSection Mutex;
		int32 Count = 0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesWireOutboundBytesTest,
	"CrowdySDK.Wire.KitEmotes.OutboundBytes", KitEventWireTestFlags)
bool FCrowdyKitEmotesWireOutboundBytesTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Json = KitEventWireAscii(KitEventWireJsonNoTarget);

	FKitEventRequest Request;
	Request.AppID = 7;
	Request.ChunkX = 1;
	Request.ChunkY = -2;
	Request.ChunkZ = 3;
	Request.ReplicationDistance = ECrowdyReplicationDistance::Two_Chunks;
	Request.DecayRate = ECrowdyDecayRate::No_Decay;
	Request.UUID = FCrowdyActorId::FromAnsiLiteral("0123456789abcdef0123456789abcdef");
	Request.EventType = CrowdyKitEvent::Emote;
	Request.State = Json;

	// The whole datagram body, written out field by field: opcode 138, the spatial header, the event type 101 as two
	// little-endian octets, then the JSON to the end of the message with no length in front and nothing after.
	TArray<uint8> Expected;
	Expected.Add(138);
	KitEventWireAppendLittleEndian64(Expected, 7);
	KitEventWireAppendLittleEndian64(Expected, 1);
	KitEventWireAppendLittleEndian64(Expected, -2);
	KitEventWireAppendLittleEndian64(Expected, 3);
	Expected.Add(2);
	Expected.Add(0);
	Expected.Add(1);
	Expected.Append(KitEventWireAscii(KitEventWireSender));
	Expected.Add(0x65);
	Expected.Add(0x00);
	Expected.Append(Json);

	const TArray<uint8> Actual = Request.Serialize();
	TestTrue(FString::Printf(TEXT("the kit event serializes to the bytes CrowdyCPP sends (%s)"),
		*CrowdyWireParity::DescribeDifference(Actual, Expected)), Actual == Expected);

	TArray<uint8> Storage;
	FCrowdyCppOutboundFrame Frame;
	FString Error;
	const bool bSplit = CrowdyCppSend::SplitMessage(Request, Storage, Frame, Error);
	if (!TestTrue(FString::Printf(TEXT("the kit event splits for the connection (%s)"), *Error), bSplit))
	{
		return false;
	}

	TestEqual(TEXT("it goes out as a client event"), static_cast<int32>(Frame.Opcode), 138);
	TestEqual(TEXT("its reach is two chunks"), static_cast<int32>(Frame.Distance), 2);
	TestEqual(TEXT("it does not decay"), static_cast<int32>(Frame.Decay), 0);

	TArray<uint8> SplitPayload(Frame.Payload.GetData(), Frame.Payload.Num());
	TArray<uint8> CrowdyCppPayload = KitEventWireCrowdyCppPayload(101, Json);
	TestTrue(FString::Printf(TEXT("the payload is what CrowdyCPP's own encoder makes of type 101 and the JSON (%s)"),
		*CrowdyWireParity::DescribeDifference(SplitPayload, CrowdyCppPayload)), SplitPayload == CrowdyCppPayload);

	// What the connection hands CrowdyCPP, read back with CrowdyCPP's own reader.
	const crowdy::Result<crowdy::wire::EventPayloadView> Event = crowdy::wire::parseEventPayload(
		crowdy::Bytes(Frame.Payload.GetData(), static_cast<std::size_t>(Frame.Payload.Num())));
	if (!TestTrue(TEXT("CrowdyCPP reads the payload as an event"), Event.ok()))
	{
		return false;
	}

	TestEqual(TEXT("CrowdyCPP reads event type 101"), static_cast<int32>(Event->eventType), 101);
	TestEqual(TEXT("CrowdyCPP's state is exactly the JSON, nothing before or after it"),
		static_cast<int32>(Event->state.size()), Json.Num());
	TestTrue(TEXT("CrowdyCPP's state bytes are the JSON bytes"),
		Event->state.size() == static_cast<std::size_t>(Json.Num())
			&& FMemory::Memcmp(Event->state.data(), Json.GetData(), Json.Num()) == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesWireInboundDecodeTest,
	"CrowdySDK.Wire.KitEmotes.InboundFromCrowdyCpp", KitEventWireTestFlags)
bool FCrowdyKitEmotesWireInboundDecodeTest::RunTest(const FString& Parameters)
{
	const FCrowdyActorId ExpectedSender = FCrowdyActorId::FromAnsiLiteral("0123456789abcdef0123456789abcdef");

	for (const ANSICHAR* JsonText : { KitEventWireJsonNoTarget, KitEventWireJsonWithTarget })
	{
		const TArray<uint8> Json = KitEventWireAscii(JsonText);
		const TArray<uint8> Stripped = KitEventWireStrippedFrame(KitEventWireCrowdyCppPayload(101, Json));

		FKitEventWireWarningCapture Capture;
		GLog->AddOutputDevice(&Capture);

		FGameEventNotification Message;
		const bool bDecoded = CrowdyWireParity::DecodeStrippedPayload(Message, Stripped);

		GLog->Flush();
		GLog->RemoveOutputDevice(&Capture);

		const FString Label(JsonText);
		if (!TestTrue(FString::Printf(TEXT("%s: an emote from CrowdyCPP decodes"), *Label), bDecoded))
		{
			continue;
		}

		TestEqual(TEXT("event type"), static_cast<int32>(Message.EventType), 101);
		TestEqual(TEXT("the sender is the header actor id"), Message.UUID, ExpectedSender);
		TestEqual(TEXT("the state is the whole JSON"), Message.StateView.Num(), Json.Num());
		TestTrue(TEXT("the state bytes are the JSON bytes"),
			Message.StateView.Num() == Json.Num()
				&& FMemory::Memcmp(Message.StateView.GetData(), Json.GetData(), Json.Num()) == 0);
		TestEqual(TEXT("the declared size follows the state"), Message.StateSize, Json.Num());
		TestFalse(TEXT("it is not read as a CrowdyEvent struct"), Message.State.IsValid());
		TestEqual(TEXT("it is addressed to everyone nearby"), static_cast<int32>(Message.Target),
			static_cast<int32>(ECrowdyTarget::Everyone));
		TestEqual(TEXT("decoding it logged no warning"), Capture.Read(), 0);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesWireOtherEventsKeepTheirLengthTest,
	"CrowdySDK.Wire.KitEmotes.OtherEventsKeepTheirLengthPrefix", KitEventWireTestFlags)
bool FCrowdyKitEmotesWireOtherEventsKeepTheirLengthTest::RunTest(const FString& Parameters)
{
	// A targeted message carrying 101 is not a kit emote: only the client event opcode is read bare, so these bytes, whose
	// first four would be a state size running past the message, are refused.
	AddExpectedMessagePlain(TEXT("Invalid StateSize"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	const TArray<uint8> Json = KitEventWireAscii(KitEventWireJsonNoTarget);
	FSingleActorNotification Targeted;
	TestFalse(TEXT("a single-actor message with type 101 still reads a length prefix"),
		CrowdyWireParity::DecodeStrippedPayload(Targeted, KitEventWireStrippedFrame(KitEventWireCrowdyCppPayload(101, Json))));

	// A client event of another number that carries its length reads it, and the target block after the state.
	TArray<uint8> Lengthed = {0x34, 0x12, 3, 0, 0, 0};
	Lengthed.Append(KitEventWireAscii("abc"));
	Lengthed.Add(static_cast<uint8>(ECrowdyTarget::Owner));
	Lengthed.Append(KitEventWireAscii("fedcba9876543210fedcba9876543210"));
	FGameEventNotification Event;
	// The decoded state is a view into these bytes, so they outlive the checks below.
	const TArray<uint8> LengthedFrame = KitEventWireStrippedFrame(Lengthed);
	if (TestTrue(TEXT("a client event with its length decodes"), CrowdyWireParity::DecodeStrippedPayload(Event, LengthedFrame)))
	{
		TestEqual(TEXT("its state is what the length says"), KitEventWireText(Event.StateView), FString(TEXT("abc")));
		TestEqual(TEXT("the declared size is read"), Event.StateSize, 3);
		TestEqual(TEXT("and the target block after it"), static_cast<int32>(Event.Target), static_cast<int32>(ECrowdyTarget::Owner));
		TestTrue(TEXT("with its target id"), Event.TargetID.IsValid());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesWireBareClientEventsTest,
	"CrowdySDK.Wire.KitEmotes.BareClientEventsFromOtherLibraries", KitEventWireTestFlags)
bool FCrowdyKitEmotesWireBareClientEventsTest::RunTest(const FString& Parameters)
{
	// Other client libraries and Mod Scripts write every event as its type and then its state, no length: CrowdyCPP's own
	// encoder. Where those bytes cannot be a length and its state, the event is read that way instead of refused.
	struct FCase
	{
		const TCHAR* Label;
		TArray<uint8> State;
	};
	const FCase Cases[] = {
		{TEXT("JSON"), KitEventWireAscii(KitEventWireJsonNoTarget)},
		{TEXT("two octets, too short for a length"), KitEventWireAscii("hi")},
		{TEXT("no state at all"), TArray<uint8>()},
		{TEXT("a negative length"), TArray<uint8>({0xff, 0xff, 0xff, 0xff, 1})},
		{TEXT("a small length that runs past the body"), TArray<uint8>({5, 0, 0, 0, 0xaa})},
		{TEXT("binary that reads as a length of 0 with octets after"), TArray<uint8>({0, 0, 0, 0, 1, 2, 3, 4})},
		{TEXT("binary that reads as a length that fits, with fewer octets after than a target block"), TArray<uint8>({2, 0, 0, 0, 0xaa, 0xbb, 0xcc, 0xdd})},
	};
	// A negative length followed by as many octets as a target block, so only the length's sign tells it apart.
	TArray<uint8> NegativeThenBlock = {0xff, 0xff, 0xff, 0xff};
	NegativeThenBlock.AddZeroed(FGameEventNotification::TargetBlockBytes - 1);
	const FCase Negative = {TEXT("a negative length with a target block's worth after it"), NegativeThenBlock};
	TArray<FCase> All(Cases, UE_ARRAY_COUNT(Cases));
	All.Add(Negative);
	for (const FCase& Case : All)
	{
		FKitEventWireWarningCapture Capture;
		GLog->AddOutputDevice(&Capture);
		FGameEventNotification Event;
		// The decoded state is a view into these bytes, so they outlive the checks below.
		const TArray<uint8> Stripped = KitEventWireStrippedFrame(KitEventWireCrowdyCppPayload(7, Case.State));
		const bool bDecoded = CrowdyWireParity::DecodeStrippedPayload(Event, Stripped);
		GLog->Flush();
		GLog->RemoveOutputDevice(&Capture);
		if (!TestTrue(FString::Printf(TEXT("%s: a bare client event decodes"), Case.Label), bDecoded))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s: its type"), Case.Label), static_cast<int32>(Event.EventType), 7);
		TestTrue(FString::Printf(TEXT("%s: its state is every octet after the type"), Case.Label),
			Event.StateView.Num() == Case.State.Num() && FMemory::Memcmp(Event.StateView.GetData(), Case.State.GetData(), Case.State.Num()) == 0);
		TestFalse(FString::Printf(TEXT("%s: it is not read as a CrowdyEvent struct"), Case.Label), Event.State.IsValid());
		TestEqual(FString::Printf(TEXT("%s: it is for everyone nearby"), Case.Label), static_cast<int32>(Event.Target), static_cast<int32>(ECrowdyTarget::Everyone));
		TestEqual(FString::Printf(TEXT("%s: with no warning"), Case.Label), Capture.Read(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesWireGeneratorSkipsEmoteTest,
	"CrowdySDK.Wire.KitEmotes.GeneratorNeverYieldsTheEmoteNumber", KitEventWireTestFlags)
bool FCrowdyKitEmotesWireGeneratorSkipsEmoteTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("101 is reserved"), FCrowdyTypeIDGenerator::IsReservedTypeID(101));
	TestFalse(TEXT("100 is not reserved"), FCrowdyTypeIDGenerator::IsReservedTypeID(100));
	TestFalse(TEXT("102 is not reserved"), FCrowdyTypeIDGenerator::IsReservedTypeID(102));

	// A hash of 100 is the one that gives 101. Its high part is 0, which gives 1 for an event.
	TestEqual(TEXT("a hash that would be 101 is read again from its high part for an event"),
		static_cast<int32>(FCrowdyTypeIDGenerator::EventTypeIDFromHash(100)), 1);

	// 100 + 100 * 65535: its low part is 101 again, so the remap settles on the last number.
	TestEqual(TEXT("a remap that lands on 101 again settles on 65535"),
		static_cast<int32>(FCrowdyTypeIDGenerator::EventTypeIDFromHash(100u + 100u * 65535u)), 65535);

	// The plain mapping persistence and actor state key on never moves, 101 included.
	TestEqual(TEXT("the plain mapping still gives 101 for a hash of 100"),
		static_cast<int32>(FCrowdyTypeIDGenerator::TypeIDFromHash(100)), 101);

	int32 EventReserved = 0;
	int32 EventChanged = 0;
	int32 PlainChanged = 0;
	for (uint32 Hash = 0; Hash < 65535u * 4u; ++Hash)
	{
		const uint32 Old = (Hash % 65535u) + 1u;
		EventReserved += FCrowdyTypeIDGenerator::EventTypeIDFromHash(Hash) == CrowdyKitEvent::Emote ? 1 : 0;
		EventChanged += (Old != 101u && FCrowdyTypeIDGenerator::EventTypeIDFromHash(Hash) != Old) ? 1 : 0;
		PlainChanged += FCrowdyTypeIDGenerator::TypeIDFromHash(Hash) != Old ? 1 : 0;
	}

	TestEqual(TEXT("no event hash yields 101"), EventReserved, 0);
	TestEqual(TEXT("no event hash but the ones that gave 101 changed number"), EventChanged, 0);
	TestEqual(TEXT("no plain hash changed number"), PlainChanged, 0);

	// An override of the reserved number is refused, so the struct gets the id its path hashes to.
	const UScriptStruct* Struct = TBaseStructure<FVector>::Get();
	AddExpectedErrorPlain(TEXT("a number the platform keeps for its own events"), EAutomationExpectedErrorFlags::Contains, 2);
	TestEqual(TEXT("an override of 101 is refused for an event"),
		static_cast<int32>(UEventPayloadRegistry::ResolveEventID(Struct, TOptional<int32>(101))),
		static_cast<int32>(FCrowdyTypeIDGenerator::GenerateEventFromStruct(Struct)));
	TestEqual(TEXT("another override is used as given"),
		static_cast<int32>(UEventPayloadRegistry::ResolveEventID(Struct, TOptional<int32>(7777))), 7777);
	TestEqual(TEXT("an override that only wraps to 101 is refused too"),
		static_cast<int32>(UEventPayloadRegistry::ResolveEventID(Struct, TOptional<int32>(65536 + 101))),
		static_cast<int32>(FCrowdyTypeIDGenerator::GenerateEventFromStruct(Struct)));
	return true;
}

#endif
