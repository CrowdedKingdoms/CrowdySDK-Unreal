#include "Subsystem/CrowdyProximityChat.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "Messages/Communication/FTextMessageNotification.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	constexpr EAutomationTestFlags ProximityChatTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	FString ProximityChatRepeat(const TCHAR Character, const int32 Count)
	{
		FString Text;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Text.AppendChar(Character);
		}
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProximityChatCheckTest,
	"CrowdySDK.Services.ProximityChat.CheckRefusesBeforeSending", ProximityChatTestFlags)
bool FCrowdyProximityChatCheckTest::RunTest(const FString& Parameters)
{
	using EResult = ECrowdyProximityChatResult;
	auto Check = [](const FString& Text, const bool bConnected = true, const bool bHasPosition = true)
	{
		return UCrowdyProximityChat::CheckText(Text, bConnected, bHasPosition);
	};

	TestEqual(TEXT("a line within every limit is sent"), Check(TEXT("hello")), EResult::Sent);
	TestEqual(TEXT("an empty line is refused"), Check(FString()), EResult::EmptyText);
	TestEqual(TEXT("an empty line is refused before the connection is asked"), Check(FString(), false, false), EResult::EmptyText);

	const int32 Max = UCrowdyProximityChat::MaxTextBytes;
	TestEqual(TEXT("exactly the limit is sent"), Check(ProximityChatRepeat(TEXT('a'), Max)), EResult::Sent);
	TestEqual(TEXT("one byte over is too long"), Check(ProximityChatRepeat(TEXT('a'), Max + 1)), EResult::TooLong);

	// The limit is in UTF-8 bytes, not characters: three bytes each, so a third of the limit is the most that fits.
	TestEqual(TEXT("multi-byte text within the limit is sent"), Check(ProximityChatRepeat(TEXT('€'), Max / 3)), EResult::Sent);
	TestEqual(TEXT("multi-byte text past the limit in bytes is too long"), Check(ProximityChatRepeat(TEXT('€'), Max / 3 + 1)), EResult::TooLong);

	TestEqual(TEXT("no connection is not connected"), Check(TEXT("hello"), false), EResult::NotConnected);
	TestEqual(TEXT("no pawn has no position"), Check(TEXT("hello"), true, false), EResult::NoPosition);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProximityChatSenderTest,
	"CrowdySDK.Services.ProximityChat.SenderNamesItsEntity", ProximityChatTestFlags)
bool FCrowdyProximityChatSenderTest::RunTest(const FString& Parameters)
{
	const FGuid PlayerEntity(0x12345678, 0x9ABCDEF0, 0x0F1E2D3C, 0x4B5A6978);
	const FString PlayerUuid = FCrowdyActorId::FromGuid(PlayerEntity).ToString();
	TestEqual(TEXT("a player's actor id names its entity"), UCrowdyProximityChat::EntityIdFromSenderUuid(PlayerUuid), PlayerEntity);
	TestEqual(TEXT("in lower case too"), UCrowdyProximityChat::EntityIdFromSenderUuid(PlayerUuid.ToLower()), PlayerEntity);
	TestFalse(TEXT("an actor id that is not hex names no entity"),
		UCrowdyProximityChat::EntityIdFromSenderUuid(TEXT("player-web-0000000000000000000000")).IsValid());
	TestFalse(TEXT("nor does an empty one"), UCrowdyProximityChat::EntityIdFromSenderUuid(FString()).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProximityChatReceiveTest,
	"CrowdySDK.Services.ProximityChat.ReceivedLineKeepsSenderAndDropsOwn", ProximityChatTestFlags)
bool FCrowdyProximityChatReceiveTest::RunTest(const FString& Parameters)
{
	const FGuid OtherEntity(1, 2, 3, 4);
	const FCrowdyActorId Other = FCrowdyActorId::FromGuid(OtherEntity);
	const FCrowdyActorId Own = FCrowdyActorId::FromGuid(FGuid(5, 6, 7, 8));

	FTextMessageNotification Notification;
	Notification.UUID = Other;
	Notification.Message = TEXT("hi\nthere");

	FCrowdyProximityText Message;
	if (TestTrue(TEXT("another player's line is delivered"), UCrowdyProximityChat::ReadReceivedText(Notification, Own, Message)))
	{
		TestEqual(TEXT("with its sender's actor id"), Message.SenderUuid, Other.ToString());
		TestEqual(TEXT("and the entity that id names"), Message.SenderEntityId, OtherEntity);
		TestEqual(TEXT("and the text exactly as it arrived"), Message.Text, FString(TEXT("hi\nthere")));
	}

	FCrowdyProximityText Unset;
	TestTrue(TEXT("with no local id yet, nothing reads as ours"), UCrowdyProximityChat::ReadReceivedText(Notification, FCrowdyActorId(), Unset));

	Notification.UUID = Own;
	FCrowdyProximityText Echo;
	TestFalse(TEXT("a line under our own id is not delivered"), UCrowdyProximityChat::ReadReceivedText(Notification, Own, Echo));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProximityChatRefusalTest,
	"CrowdySDK.Services.ProximityChat.OnlyTextRefusalsAreReported", ProximityChatTestFlags)
bool FCrowdyProximityChatRefusalTest::RunTest(const FString& Parameters)
{
	FCrowdySendRefusal Refusal;
	Refusal.Code = 7;
	Refusal.MessageType = static_cast<int32>(ECrowdyMessageType::CLIENT_TEXT_PACKET);
	TestTrue(TEXT("a refused proximity text is reported"), UCrowdyProximityChat::IsTextRefusal(Refusal));

	Refusal.bChannelMessage = true;
	Refusal.ChannelId = 9;
	TestFalse(TEXT("a refused channel message is not"), UCrowdyProximityChat::IsTextRefusal(Refusal));

	Refusal.bChannelMessage = false;
	Refusal.ChannelId = 0;
	Refusal.MessageType = static_cast<int32>(ECrowdyMessageType::CLIENT_AUDIO_PACKET);
	TestFalse(TEXT("nor is a refused voice packet"), UCrowdyProximityChat::IsTextRefusal(Refusal));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyProximityChatOfflineTest,
	"CrowdySDK.Services.ProximityChat.SendWithoutConnectionSendsNothing", ProximityChatTestFlags)
bool FCrowdyProximityChatOfflineTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UCrowdyProximityChat> Chat(NewObject<UCrowdyProximityChat>(Instance.Get()));

	TestEqual(TEXT("an empty line is refused"), Chat->SendProximityText(FString(), 4), ECrowdyProximityChatResult::EmptyText);
	TestEqual(TEXT("a line with no connection is not sent"), Chat->SendProximityText(TEXT("hello"), 4),
		ECrowdyProximityChatResult::NotConnected);
	return true;
}

#endif
