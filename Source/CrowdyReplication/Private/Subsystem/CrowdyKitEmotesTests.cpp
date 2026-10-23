#include "Subsystem/CrowdyKitEmotes.h"
#include "Subsystem/CrowdyKitEmotesTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Core/UDP/CrowdyKitEvents.h"
#include "Core/UDP/Subscription/FCrowdyDelivery.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Messages/GameObjects/FKitEventRequest.h"
#include "Messages/GameObjects/FServerEventNotification.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Misc/AutomationTest.h"
#include "Network/UDP/CrowdyCppSendAdapter.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Utils/SerializationFunctionLibrary.h"

namespace
{
	constexpr EAutomationTestFlags KitEmotesTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	constexpr const TCHAR* KitEmotesTestTargetUuid = TEXT("fedcba9876543210fedcba9876543210");

	TArray<uint8> KitEmotesTestUtf8(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	bool KitEmotesTestDecode(const FString& Json, FString& OutEmote, FString& OutTargetUuid)
	{
		return UCrowdyKitEmotes::DecodeEmoteBody(KitEmotesTestUtf8(Json), OutEmote, OutTargetUuid);
	}

	FCrowdyActorId KitEmotesTestSender()
	{
		return FCrowdyActorId::FromAnsiLiteral("0123456789abcdef0123456789abcdef");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesEncodeMatchesTheKitTest,
	"CrowdySDK.Replication.KitEmotes.EncodeMatchesTheKit", KitEmotesTestFlags)
bool FCrowdyKitEmotesEncodeMatchesTheKitTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Body;
	FString Reason;

	// The text CrowdyCPP's encodeEmoteBody writes: keys sorted, no spaces, targetUuid always present.
	TestTrue(TEXT("an emote with no target encodes"), UCrowdyKitEmotes::EncodeEmoteBody(TEXT("wave"), FString(), Body, Reason));
	TestTrue(TEXT("no target writes null"),
		Body == KitEmotesTestUtf8(TEXT("{\"emote\":\"wave\",\"targetUuid\":null}")));

	TestTrue(TEXT("an emote with a target encodes"),
		UCrowdyKitEmotes::EncodeEmoteBody(TEXT("wave"), KitEmotesTestTargetUuid, Body, Reason));
	TestTrue(TEXT("the target is written as the uuid text"),
		Body == KitEmotesTestUtf8(TEXT("{\"emote\":\"wave\",\"targetUuid\":\"fedcba9876543210fedcba9876543210\"}")));

	FString Tricky = TEXT("say \"hi\"\n\\ ");
	Tricky.AppendChar(static_cast<TCHAR>(1));
	Tricky.AppendChar(static_cast<TCHAR>(0x00e9));

	FString TrickyJson = TEXT("{\"emote\":\"say \\\"hi\\\"\\n\\\\ \\u0001");
	TrickyJson.AppendChar(static_cast<TCHAR>(0x00e9));
	TrickyJson += TEXT("\",\"targetUuid\":null}");

	TestTrue(TEXT("quotes, backslashes and control characters are escaped as the kit escapes them"),
		UCrowdyKitEmotes::EncodeEmoteBody(Tricky, FString(), Body, Reason));
	TestTrue(TEXT("and text outside ASCII stays as UTF-8"), Body == KitEmotesTestUtf8(TrickyJson));

	FString RoundEmote;
	FString RoundTarget;
	TestTrue(TEXT("what is encoded decodes"), UCrowdyKitEmotes::DecodeEmoteBody(Body, RoundEmote, RoundTarget));
	TestEqual(TEXT("to the same emote"), RoundEmote, Tricky);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesEncodeRefusalsTest,
	"CrowdySDK.Replication.KitEmotes.EncodeRefusesWhatDoesNotFit", KitEmotesTestFlags)
bool FCrowdyKitEmotesEncodeRefusalsTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Body;
	FString Reason;

	TestFalse(TEXT("an empty emote is refused"), UCrowdyKitEmotes::EncodeEmoteBody(FString(), FString(), Body, Reason));
	TestFalse(TEXT("with a reason"), Reason.IsEmpty());
	TestEqual(TEXT("and no body"), Body.Num(), 0);

	TestTrue(TEXT("a one character emote encodes"), UCrowdyKitEmotes::EncodeEmoteBody(TEXT("a"), FString(), Body, Reason));
	const int32 Overhead = Body.Num() - 1;
	const int32 Fit = CrowdyKitEvent::MaxEmoteStateBytes - Overhead;

	Reason.Reset();
	TestTrue(TEXT("a body of exactly 1024 bytes is accepted"),
		UCrowdyKitEmotes::EncodeEmoteBody(FString::ChrN(Fit, TEXT('a')), FString(), Body, Reason));
	TestEqual(TEXT("and it is 1024 bytes"), Body.Num(), CrowdyKitEvent::MaxEmoteStateBytes);

	TestFalse(TEXT("a body of 1025 bytes is refused"),
		UCrowdyKitEmotes::EncodeEmoteBody(FString::ChrN(Fit + 1, TEXT('a')), FString(), Body, Reason));
	TestFalse(TEXT("with a reason that names the limit and what it covers"),
		!Reason.Contains(TEXT("1024")) || !Reason.Contains(TEXT("emote and its target")));
	TestEqual(TEXT("and no body is left behind"), Body.Num(), 0);

	// The limit counts octets, not characters: this many two-octet characters is under 1024 of them and over 1024 bytes.
	TestTrue(TEXT("two-octet characters that fit are accepted"),
		UCrowdyKitEmotes::EncodeEmoteBody(FString::ChrN(Fit / 2, static_cast<TCHAR>(0x00e9)), FString(), Body, Reason));
	TestFalse(TEXT("one more of them is refused"),
		UCrowdyKitEmotes::EncodeEmoteBody(FString::ChrN(Fit / 2 + 1, static_cast<TCHAR>(0x00e9)), FString(), Body, Reason));

	TestFalse(TEXT("a very long emote is refused"),
		UCrowdyKitEmotes::EncodeEmoteBody(FString::ChrN(5000, TEXT('a')), FString(), Body, Reason));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesDecodeTest,
	"CrowdySDK.Replication.KitEmotes.DecodeReadsAnyLibrarysBody", KitEmotesTestFlags)
bool FCrowdyKitEmotesDecodeTest::RunTest(const FString& Parameters)
{
	FString Emote;
	FString Target;

	TestTrue(TEXT("CrowdyCPP's body with a null target decodes"),
		KitEmotesTestDecode(TEXT("{\"emote\":\"wave\",\"targetUuid\":null}"), Emote, Target));
	TestEqual(TEXT("emote"), Emote, FString(TEXT("wave")));
	TestTrue(TEXT("a null target reads as empty"), Target.IsEmpty());

	TestTrue(TEXT("CrowdyCPP's body with a target decodes"),
		KitEmotesTestDecode(TEXT("{\"emote\":\"cheer\",\"targetUuid\":\"fedcba9876543210fedcba9876543210\"}"), Emote, Target));
	TestEqual(TEXT("emote with a target"), Emote, FString(TEXT("cheer")));
	TestEqual(TEXT("target"), Target, FString(KitEmotesTestTargetUuid));

	TestTrue(TEXT("keys in another order, with spaces and an extra field, decode"),
		KitEmotesTestDecode(TEXT("{ \"extra\": 1, \"targetUuid\": \"abc\", \"emote\": \"bow\" }"), Emote, Target));
	TestEqual(TEXT("emote in another order"), Emote, FString(TEXT("bow")));
	TestEqual(TEXT("target in another order"), Target, FString(TEXT("abc")));

	TestTrue(TEXT("a body with no targetUuid decodes"), KitEmotesTestDecode(TEXT("{\"emote\":\"dance\"}"), Emote, Target));
	TestTrue(TEXT("and has no target"), Target.IsEmpty());

	TestTrue(TEXT("a targetUuid that is not text reads as no target"),
		KitEmotesTestDecode(TEXT("{\"emote\":\"dance\",\"targetUuid\":7}"), Emote, Target));
	TestTrue(TEXT("and has no target when it is a number"), Target.IsEmpty());

	TestTrue(TEXT("an escaped emote decodes"),
		KitEmotesTestDecode(TEXT("{\"emote\":\"a\\u00e9\\n\"}"), Emote, Target));
	FString Expected = TEXT("a");
	Expected.AppendChar(static_cast<TCHAR>(0x00e9));
	Expected.AppendChar(TEXT('\n'));
	TestEqual(TEXT("with its escapes read"), Emote, Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesDecodeDropsTest,
	"CrowdySDK.Replication.KitEmotes.DecodeDropsMalformedBodies", KitEmotesTestFlags)
bool FCrowdyKitEmotesDecodeDropsTest::RunTest(const FString& Parameters)
{
	FString Emote;
	FString Target;

	const TArray<FString> Malformed = {
		TEXT(""),
		TEXT("wave"),
		TEXT("{"),
		TEXT("{\"emote\":\"wa"),
		TEXT("[\"wave\"]"),
		TEXT("\"wave\""),
		TEXT("{}"),
		TEXT("{\"targetUuid\":null}"),
		TEXT("{\"emote\":null}"),
		TEXT("{\"emote\":5}"),
		TEXT("{\"emote\":true}"),
		TEXT("{\"emote\":[\"wave\"]}"),
		TEXT("{\"emote\":{\"name\":\"wave\"}}"),
	};

	for (const FString& Json : Malformed)
	{
		TestFalse(FString::Printf(TEXT("'%s' is dropped"), *Json), KitEmotesTestDecode(Json, Emote, Target));
		TestTrue(FString::Printf(TEXT("'%s' leaves nothing behind"), *Json), Emote.IsEmpty() && Target.IsEmpty());
	}

	const FString TooBig = TEXT("{\"emote\":\"") + FString::ChrN(CrowdyKitEvent::MaxEmoteStateBytes, TEXT('a')) + TEXT("\"}");
	TestFalse(TEXT("a body over 1024 bytes is dropped"), KitEmotesTestDecode(TooBig, Emote, Target));

	const FString Deep = TEXT("{\"emote\":\"a\",\"x\":") + FString::ChrN(100, TEXT('[')) + FString::ChrN(100, TEXT(']')) + TEXT("}");
	TestTrue(TEXT("the deep body is small enough to be read"), Deep.Len() < CrowdyKitEvent::MaxEmoteStateBytes);
	TestFalse(TEXT("a body nested past the depth limit is dropped"), KitEmotesTestDecode(Deep, Emote, Target));

	const uint8 NotText[] = { 0xff, 0xfe, 0x00, 0x7b };
	TestFalse(TEXT("octets that are not JSON are dropped"),
		UCrowdyKitEmotes::DecodeEmoteBody(TConstArrayView<uint8>(NotText, UE_ARRAY_COUNT(NotText)), Emote, Target));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesRequestTest,
	"CrowdySDK.Replication.KitEmotes.RequestIsTheKitsEvent", KitEmotesTestFlags)
bool FCrowdyKitEmotesRequestTest::RunTest(const FString& Parameters)
{
	TArray<uint8> Body;
	FString Reason;
	TestTrue(TEXT("the body encodes"), UCrowdyKitEmotes::EncodeEmoteBody(TEXT("wave"), FString(), Body, Reason));

	FKitEventRequest Request;
	UCrowdyKitEmotes::FillEmoteRequest(Request, 7, KitEmotesTestSender(), 1, -2, 3, CopyTemp(Body));

	TArray<uint8> Storage;
	FCrowdyCppOutboundFrame Frame;
	FString Error;
	const bool bSplit = CrowdyCppSend::SplitMessage(Request, Storage, Frame, Error);
	if (!TestTrue(FString::Printf(TEXT("the request splits (%s)"), *Error), bSplit))
	{
		return false;
	}

	TestEqual(TEXT("it is a client event"), static_cast<int32>(Frame.Opcode), 138);
	TestEqual(TEXT("app id"), Frame.AppId, static_cast<int64>(7));
	TestEqual(TEXT("chunk x"), Frame.ChunkX, static_cast<int64>(1));
	TestEqual(TEXT("chunk y"), Frame.ChunkY, static_cast<int64>(-2));
	TestEqual(TEXT("chunk z"), Frame.ChunkZ, static_cast<int64>(3));
	TestEqual(TEXT("it reaches two chunks"), static_cast<int32>(Frame.Distance), 2);
	TestEqual(TEXT("it does not decay"), static_cast<int32>(Frame.Decay), 0);
	TestTrue(TEXT("it is sent from the player's actor id"),
		FCrowdyActorId::FromOctets(Frame.Uuid) == KitEmotesTestSender());

	TArray<uint8> Expected;
	Expected.Add(0x65);
	Expected.Add(0x00);
	Expected.Append(Body);
	const TArray<uint8> Payload(Frame.Payload.GetData(), Frame.Payload.Num());
	TestTrue(TEXT("the payload is event type 101 then the JSON, with no length and no routing block"), Payload == Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesReceiveTest,
	"CrowdySDK.Replication.KitEmotes.ReceiveBroadcastsOnEmote", KitEmotesTestFlags)
bool FCrowdyKitEmotesReceiveTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UCrowdyKitEmotes> Emotes(NewObject<UCrowdyKitEmotes>(GetTransientPackage()));
	TStrongObjectPtr<UCrowdyKitEmotesTestListener> Listener(NewObject<UCrowdyKitEmotesTestListener>(GetTransientPackage()));
	Emotes->OnEmote.AddDynamic(Listener.Get(), &UCrowdyKitEmotesTestListener::OnEmote);

	TArray<uint8> Body;
	FString Reason;
	TestTrue(TEXT("the body encodes"),
		UCrowdyKitEmotes::EncodeEmoteBody(TEXT("wave"), KitEmotesTestTargetUuid, Body, Reason));

	Emotes->ReceiveEmote(KitEmotesTestSender(), Body);
	TestEqual(TEXT("an emote fires OnEmote once"), Listener->Calls, 1);
	TestEqual(TEXT("with the sender's uuid"), Listener->LastSenderUuid, FString(TEXT("0123456789abcdef0123456789abcdef")));
	TestEqual(TEXT("the emote"), Listener->LastEmote, FString(TEXT("wave")));
	TestEqual(TEXT("the target's uuid"), Listener->LastTargetUuid, FString(KitEmotesTestTargetUuid));
	TestNull(TEXT("no sender actor without a world to look it up in"), Listener->LastSenderActor);
	TestNull(TEXT("and no target actor"), Listener->LastTargetActor);

	const TArray<uint8> Garbage = KitEmotesTestUtf8(TEXT("{\"emote\":5}"));
	Emotes->ReceiveEmote(KitEmotesTestSender(), Garbage);
	TestEqual(TEXT("a body with a non-text emote fires nothing"), Listener->Calls, 1);

	// The subscription's entry point: a client event is read, anything else that shares the number is not.
	TSharedRef<FGameEventNotification, ESPMode::ThreadSafe> ClientEvent = MakeShared<FGameEventNotification, ESPMode::ThreadSafe>();
	ClientEvent->UUID = KitEmotesTestSender();
	ClientEvent->EventType = CrowdyKitEvent::Emote;
	ClientEvent->StateView = Body;
	const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe> ClientMessage = ClientEvent;
	const FCrowdyDelivery ClientDelivery(ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION, ClientEvent->GetPayloadKey(),
		nullptr, ClientEvent->GetPayloadBytes(), ClientMessage);

	Emotes->HandleDelivery(ClientDelivery);
	TestEqual(TEXT("a client event delivery fires OnEmote"), Listener->Calls, 2);

	TSharedRef<FServerEventNotification, ESPMode::ThreadSafe> ServerEvent = MakeShared<FServerEventNotification, ESPMode::ThreadSafe>();
	ServerEvent->UUID = KitEmotesTestSender();
	ServerEvent->EventType = CrowdyKitEvent::Emote;
	ServerEvent->StateView = Body;
	const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe> ServerMessage = ServerEvent;
	const FCrowdyDelivery ServerDelivery(ECrowdyMessageType::SERVER_EVENT_NOTIFICATION, ServerEvent->GetPayloadKey(),
		nullptr, ServerEvent->GetPayloadBytes(), ServerMessage);

	Emotes->HandleDelivery(ServerDelivery);
	TestEqual(TEXT("a server event numbered 101 is not an emote"), Listener->Calls, 2);

	// Listeners are handed the uuid text exactly as the sender wrote it, case included.
	const FString MixedCase = TEXT("ABCDEF0123456789abcdef0123456789");
	TestTrue(TEXT("a body with a mixed case target encodes"),
		UCrowdyKitEmotes::EncodeEmoteBody(TEXT("wave"), MixedCase, Body, Reason));
	Emotes->ReceiveEmote(FCrowdyActorId::FromAnsiLiteral("FEDCBA9876543210fedcba9876543210"), Body);
	TestEqual(TEXT("the sender uuid is as received"), Listener->LastSenderUuid, FString(TEXT("FEDCBA9876543210fedcba9876543210")));
	TestTrue(TEXT("the target uuid is as received, case included"), Listener->LastTargetUuid.Equals(MixedCase, ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesWireIdTest,
	"CrowdySDK.Replication.KitEmotes.TargetIsSentAsItsWireText", KitEmotesTestFlags)
bool FCrowdyKitEmotesWireIdTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyActorTracker> Tracker(NewObject<UCrowdyActorTracker>(GetTransientPackage()));
	Tracker->Configure(64, 8, 0.002f, 12.0f);

	// A hex id in the case it arrived in, and an id that is not hex at all: neither can be rebuilt from its key.
	const FCrowdyActorId Upper = FCrowdyActorId::FromAnsiLiteral("0123456789ABCDEF0123456789ABCDEF");
	const FCrowdyActorId Lower = FCrowdyActorId::FromAnsiLiteral("fedcba9876543210fedcba9876543210");
	const FCrowdyActorId Mob = FCrowdyActorId::FromAnsiLiteral("M10,-4@3                        ");

	TArray<FCrowdyActorUpdate> Appeared;
	for (const FCrowdyActorId& WireId : { Upper, Lower, Mob })
	{
		TSharedRef<FActorUpdateNotificationMessage, ESPMode::ThreadSafe> Message = MakeShared<FActorUpdateNotificationMessage, ESPMode::ThreadSafe>();
		Message->UUID = WireId;
		FCrowdyActorUpdate& Update = Appeared.AddDefaulted_GetRef();
		Update.UUID = USerializationFunctionLibrary::ToGuid(WireId);
		Update.Message = Message;
	}
	Tracker->RecordAppearancesForTest(Appeared);

	FCrowdyActorId Found;
	TestTrue(TEXT("an uppercase hex id is found"), Tracker->FindWireId(USerializationFunctionLibrary::ToGuid(Upper), Found));
	TestEqual(TEXT("and comes back uppercase"), Found.ToString(), Upper.ToString());
	TestTrue(TEXT("a lowercase hex id is found"), Tracker->FindWireId(USerializationFunctionLibrary::ToGuid(Lower), Found));
	TestEqual(TEXT("and comes back lowercase"), Found.ToString(), Lower.ToString());
	TestTrue(TEXT("a non-hex id is found"), Tracker->FindWireId(USerializationFunctionLibrary::ToGuid(Mob), Found));
	TestTrue(TEXT("and comes back as written"), Found.ToString().Equals(Mob.ToString(), ESearchCase::CaseSensitive));
	TestTrue(TEXT("with its octets intact"), Found == Mob);

	Found.Reset();
	TestFalse(TEXT("an actor that never appeared is not found"), Tracker->FindWireId(FGuid(1, 2, 3, 4), Found));
	TestFalse(TEXT("and leaves nothing behind"), Found.IsSet());

	Tracker->ForgetTrackedActorForTest(USerializationFunctionLibrary::ToGuid(Lower));
	TestFalse(TEXT("a forgotten actor is gone"), Tracker->FindWireId(USerializationFunctionLibrary::ToGuid(Lower), Found));
	TestTrue(TEXT("and the others stay"), Tracker->FindWireId(USerializationFunctionLibrary::ToGuid(Upper), Found));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKitEmotesSendRefusesTest,
	"CrowdySDK.Replication.KitEmotes.SendRefusesPlainly", KitEmotesTestFlags)
bool FCrowdyKitEmotesSendRefusesTest::RunTest(const FString& Parameters)
{
	FString Reason;
	TestFalse(TEXT("sending with no world is refused"),
		UCrowdyKitEmotes::SendEmoteInWorld(nullptr, TEXT("wave"), nullptr, Reason));
	TestFalse(TEXT("with a reason"), Reason.IsEmpty());

	TStrongObjectPtr<UCrowdyKitEmotes> Emotes(NewObject<UCrowdyKitEmotes>(GetTransientPackage()));
	Reason = TEXT("stale");
	TestFalse(TEXT("sending from a subsystem with no game instance is refused"), Emotes->SendEmote(TEXT("wave"), nullptr, Reason));
	TestFalse(TEXT("with a fresh reason"), Reason.IsEmpty() || Reason == TEXT("stale"));
	return true;
}

#endif
