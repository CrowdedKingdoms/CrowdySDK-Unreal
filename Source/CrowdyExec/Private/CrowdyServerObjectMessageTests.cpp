#include "CrowdyServerObjectTestTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CrowdyExecCodec.h"
#include "CrowdyNativeExec.h"
#include "CrowdyServerObject.h"
#include "CrowdyServerObjectComponent.h"
#include "CrowdyServerObjectDefinition.h"
#include "CrowdyServerObjectLibrary.h"
#include "CrowdyServerObjectLink.h"
#include "CrowdyServerObjectSubsystem.h"
#include "CrowdyServerObjectTestRig.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

/** Reaches the component's private join, which a component outside a world cannot reach through BeginPlay. */
struct FCrowdyServerObjectMessageTestAccess
{
	static void UseServerObjects(UCrowdyServerObjectComponent& Component, UCrowdyServerObjectSubsystem* Subsystem) { Component.ServerObjectsForTest = Subsystem; }
	static void Join(UCrowdyServerObjectComponent& Component) { Component.Join(); }
};

namespace CrowdyServerObjectMessageTests
{
	using namespace CrowdyServerObjectTests;

	inline constexpr const TCHAR* StructName = TEXT("BossHit");
	inline constexpr const TCHAR* StructTopic = TEXT("boss_hit");
	inline constexpr const TCHAR* ListName = TEXT("PhaseShift");
	inline constexpr const TCHAR* ListTopic = TEXT("phase_shift");
	inline constexpr const TCHAR* PlainName = TEXT("BossDefeated");
	inline constexpr const TCHAR* PlainTopic = TEXT("boss_defeated");
	inline constexpr const TCHAR* BoundFunction = TEXT("HandleMessagePayload");
	const TArray<FString> HealthOnly = {TEXT("Health")};

	/** Gives the rig's boss a message with a struct payload, one with a List payload of the same two values, and one with no payload. */
	bool AddMessages(FRig& Rig)
	{
		UCrowdyServerObjectDefinition& Definition = *Rig.Definition;
		FCrowdyServerMessage& Hit = Definition.Messages.AddDefaulted_GetRef();
		Hit.Name = StructName;
		Hit.Payload = FCrowdyServerObjectTestMessagePayload::StaticStruct();
		FCrowdyServerMessage& Shift = Definition.Messages.AddDefaulted_GetRef();
		Shift.Name = ListName;
		Shift.PayloadForm = ECrowdyServerValuesForm::List;
		Shift.PayloadList.AddProperty(TEXT("Amount"), EPropertyBagPropertyType::Int32);
		Shift.PayloadList.AddProperty(TEXT("Source"), EPropertyBagPropertyType::String);
		Definition.Messages.AddDefaulted_GetRef().Name = PlainName;
		TArray<FString> Errors;
		return Rig.Test.TestTrue(FString::Printf(TEXT("the boss with messages bakes (%s)"), *FString::Join(Errors, TEXT("; "))), Definition.Bake(Errors));
	}

	FCrowdyServerObjectTestMessagePayload MakeMessagePayload(int32 Amount, const TCHAR* Source)
	{
		FCrowdyServerObjectTestMessagePayload Value;
		Value.Amount = Amount;
		Value.Source = Source;
		return Value;
	}

	UCrowdyServerObject* ReadyBoss(FRig& Rig, const UObject* Owner)
	{
		return Rig.AcquireReady(Owner, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5);
	}

	TStrongObjectPtr<UCrowdyServerObjectTestListener> MessageListener()
	{
		return TStrongObjectPtr<UCrowdyServerObjectTestListener>(NewObject<UCrowdyServerObjectTestListener>(GetTransientPackage(), NAME_None, RF_Transient));
	}

	/** Records every message a C++ listener hears. */
	struct FMessageSpy
	{
		TSharedRef<TArray<TPair<FName, FInstancedStruct>>> Heard = MakeShared<TArray<TPair<FName, FInstancedStruct>>>();

		FOnCrowdyServerMessage::FDelegate Listener() const
		{
			TSharedRef<TArray<TPair<FName, FInstancedStruct>>> Out = Heard;
			return FOnCrowdyServerMessage::FDelegate::CreateLambda([Out](UCrowdyServerObject*, FName Message, const FInstancedStruct& Value)
			{
				Out->Emplace(Message, Value);
			});
		}

		int32 Num() const { return Heard->Num(); }
	};

	FCrowdyServerMessageEvent EventOf(UCrowdyServerObjectTestListener* Listener)
	{
		FCrowdyServerMessageEvent Event;
		Event.BindDynamic(Listener, &UCrowdyServerObjectTestListener::HandleMessageEvent);
		return Event;
	}

	int32 LastFrameIndex(const FRig& Rig, uint8 Kind, const FString& Name)
	{
		return Rig.Sent.FindLastByPredicate([Kind, &Name](const FSentFrame& Frame) { return Frame.Kind == Kind && Frame.Name.Equals(Name, ESearchCase::CaseSensitive); });
	}

	/** Drops the socket and opens the redial, which the SDK sees as a reconnect. */
	bool Reconnect(FRig& Rig)
	{
		Rig.Client->TestExecCloseFromServer(1006, false);
		if (!Rig.Test.TestTrue(TEXT("the dropped socket is redialled"), Rig.PumpUntil([&Rig]() { return Rig.Client->NumTestExecConnections() >= 2; }, 6.0)))
		{
			return false;
		}
		Rig.Client->TestExecOpen();
		return true;
	}

	/** Acknowledges every subscribe of Topic sent so far, then forgets all the frames sent. */
	void AckEvery(FRig& Rig, const FString& Topic)
	{
		Rig.PumpFor(0.2);
		for (int32 Index = Rig.FindFrame(KindSubscribe, Topic); Index != INDEX_NONE; Index = Rig.FindFrame(KindSubscribe, Topic))
		{
			Rig.Reply(Rig.Sent[Index].Rid, ECrowdyNativeExecStatus::Ok, {});
			Rig.Sent.RemoveAt(Index);
		}
		Rig.Sent.Reset();
	}

	TArray<uint8> PushPayload(FRig& Rig, int32 Amount, const TCHAR* Source)
	{
		return Rig.Encode(MakeMessagePayload(Amount, Source));
	}

	bool IsPayload(const FInstancedStruct& Value, int32 Amount, const TCHAR* Source)
	{
		const FCrowdyServerObjectTestMessagePayload* Held = Value.GetPtr<FCrowdyServerObjectTestMessagePayload>();
		return Held && Held->Amount == Amount && Held->Source == Source;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageSubscribesTopicTest,
	"CrowdySDK.CrowdyExec.ServerMessage.WatchingSubscribesTheMessageTopic", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageSubscribesTopicTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	FSentFrame Subscribe;
	if (!TestTrue(TEXT("watching a message subscribes the topic the definition names"), Rig.WaitForFrame(KindSubscribe, StructTopic, Subscribe)))
	{
		return false;
	}
	TestTrue(TEXT("on this instance"), Rig.IsAddressed(Subscribe));
	TestFalse(TEXT("without subscribing the watched values again"), Rig.SentAny(KindSubscribe, TEXT("state")));
	TestFalse(TEXT("or any other message"), Rig.SentAny(KindSubscribe, ListTopic));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageSharesOneSubscriptionTest,
	"CrowdySDK.CrowdyExec.ServerMessage.SecondListenerDoesNotSubscribeAgain", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageSharesOneSubscriptionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();
	FMessageSpy First;
	FMessageSpy Second;
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Blueprint = MessageListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Bound = MessageListener();
	Object->WatchMessage(StructName, First.Listener());
	Object->WatchMessage(StructName, Second.Listener());
	Object->K2_WatchMessage(StructName, EventOf(Blueprint.Get()));
	UCrowdyServerObjectLibrary::BindServerMessage(Object, Rig.Definition.Get(), StructName, Bound.Get(), BoundFunction);
	if (!TestTrue(TEXT("the first listener subscribes"), Rig.PumpUntil([&Rig]() { return Rig.FindFrame(KindSubscribe, StructTopic) != INDEX_NONE; })))
	{
		return false;
	}
	Rig.PumpFor(0.15);
	// The client library folds repeated subscribes into one frame, so the object's own calls are counted.
	TestEqual(TEXT("and the others, of every kind, share that subscription"), static_cast<int32>(Object->NumMessageSubscribesForTest()), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageLastUnwatchUnsubscribesTest,
	"CrowdySDK.CrowdyExec.ServerMessage.LastUnwatchUnsubscribes", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageLastUnwatchUnsubscribesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();
	FMessageSpy Spy;
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Blueprint = MessageListener();
	const FDelegateHandle Handle = Object->WatchMessage(StructName, Spy.Listener());
	Object->K2_WatchMessage(StructName, EventOf(Blueprint.Get()));
	if (!Rig.AckTopic(StructTopic))
	{
		return false;
	}

	Object->UnwatchMessage(StructName, Handle);
	TestFalse(TEXT("one listener leaving keeps the subscription for the other"), Rig.SentAny(KindUnsubscribe, StructTopic));
	Object->K2_UnwatchMessage(StructName, EventOf(Blueprint.Get()));
	FSentFrame Unsubscribe;
	if (!TestTrue(TEXT("the last one leaving unsubscribes the topic"), Rig.WaitForFrame(KindUnsubscribe, StructTopic, Unsubscribe)))
	{
		return false;
	}
	TestTrue(TEXT("on this instance"), Rig.IsAddressed(Unsubscribe));

	Object->WatchMessage(StructName, Spy.Listener());
	TestTrue(TEXT("and watching again subscribes again"), Rig.WaitForFrame(KindSubscribe, StructTopic, Unsubscribe));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageBeforeConnectionTest,
	"CrowdySDK.CrowdyExec.ServerMessage.ListenerAddedBeforeTheConnectionOpensSubscribesWhenItDoes", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageBeforeConnectionTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	TestFalse(TEXT("nothing is subscribed while there is no connection"), Rig.SentAny(KindSubscribe, StructTopic));
	if (!Rig.Open())
	{
		return false;
	}
	FSentFrame Subscribe;
	if (TestTrue(TEXT("the open connection subscribes the topic"), Rig.WaitForFrame(KindSubscribe, StructTopic, Subscribe)))
	{
		TestTrue(TEXT("on this instance"), Rig.IsAddressed(Subscribe));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageReconnectTest,
	"CrowdySDK.CrowdyExec.ServerMessage.ReconnectSubscribesAgain", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageReconnectTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	if (!Rig.AckTopic(StructTopic))
	{
		return false;
	}
	Rig.Sent.Reset();

	if (!Reconnect(Rig) || !TestTrue(TEXT("the reconnect ends the old topic subscription"),
		Rig.PumpUntil([&Rig]() { return Rig.FindFrame(KindUnsubscribe, StructTopic) != INDEX_NONE; }, 5.0)))
	{
		return false;
	}
	const int32 Unsubscribed = Rig.FindFrame(KindUnsubscribe, StructTopic);
	TestTrue(TEXT("and subscribes afresh after it, for a real answer"),
		Rig.PumpUntil([&Rig, Unsubscribed]() { return LastFrameIndex(Rig, KindSubscribe, StructTopic) > Unsubscribed; }, 5.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessagePushRunsEveryListenerTest,
	"CrowdySDK.CrowdyExec.ServerMessage.PushRunsEveryKindOfListenerWithTheDecodedPayload", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessagePushRunsEveryListenerTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Cpp;
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Blueprint = MessageListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Bound = MessageListener();
	Object->WatchMessage(StructName, Cpp.Listener());
	Object->K2_WatchMessage(StructName, EventOf(Blueprint.Get()));
	UCrowdyServerObjectLibrary::BindServerMessage(Object, Rig.Definition.Get(), StructName, Bound.Get(), BoundFunction);
	Object->WatchMessage(ListName, Cpp.Listener());
	Object->WatchMessage(PlainName, Cpp.Listener());
	if (!Rig.AckTopic(StructTopic) || !Rig.AckTopic(ListTopic) || !Rig.AckTopic(PlainTopic))
	{
		return false;
	}

	Rig.PushTopic(StructTopic, PushPayload(Rig, 25, TEXT("axe")));
	if (!TestTrue(TEXT("a struct message reaches the C++, Blueprint and bound listeners"),
		Rig.PumpUntil([&]() { return Cpp.Num() >= 1 && Blueprint->EventPayloads.Num() >= 1 && Bound->MessagePayloads.Num() >= 1; })))
	{
		return false;
	}
	Rig.PumpFor(0.1);
	TestTrue(TEXT("each hearing it once"), Cpp.Num() == 1 && Blueprint->EventPayloads.Num() == 1 && Bound->MessagePayloads.Num() == 1);
	TestTrue(TEXT("the C++ listener hears the payload decoded into the message's struct, under the message's name"),
		(*Cpp.Heard)[0].Key == FName(StructName) && IsPayload((*Cpp.Heard)[0].Value, 25, TEXT("axe")));
	TestTrue(TEXT("and so does the Blueprint event, with the object and message"), IsPayload(Blueprint->EventPayloads[0], 25, TEXT("axe")) && Blueprint->EventMessages[0] == FName(StructName));
	TestTrue(TEXT("and the bound handler"), IsPayload(Bound->MessagePayloads[0], 25, TEXT("axe")));

	Rig.PushTopic(ListTopic, PushPayload(Rig, 7, TEXT("floor")));
	if (!TestTrue(TEXT("a List message is heard"), Rig.PumpUntil([&Cpp]() { return Cpp.Num() >= 2; })))
	{
		return false;
	}
	const FInstancedPropertyBag Shift = CrowdyExec::ToList((*Cpp.Heard)[1].Value);
	const TValueOrError<int32, EPropertyBagResult> Amount = Shift.GetValueInt32(TEXT("Amount"));
	const TValueOrError<FString, EPropertyBagResult> Source = Shift.GetValueString(TEXT("Source"));
	TestTrue(TEXT("with its values read by name"), Amount.HasValue() && Amount.GetValue() == 7 && Source.HasValue() && Source.GetValue() == TEXT("floor"));
	TestEqual(TEXT("and only the listeners of its own message hear it"), Blueprint->EventPayloads.Num(), 1);

	Rig.PushTopic(PlainTopic, {0xc0});
	if (!TestTrue(TEXT("a message with no payload is heard"), Rig.PumpUntil([&Cpp]() { return Cpp.Num() >= 3; })))
	{
		return false;
	}
	TestTrue(TEXT("with an empty payload"), (*Cpp.Heard)[2].Key == FName(PlainName) && !(*Cpp.Heard)[2].Value.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageStatePushStillAppliesTest,
	"CrowdySDK.CrowdyExec.ServerMessage.StatePushStillAppliesBesideAMessageListener", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageStatePushStillAppliesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	if (!Rig.AckTopic(StructTopic))
	{
		return false;
	}
	FValuesSpy Values;
	Values.Watch(Object);

	Rig.Push(Rig.PushMessage(Epoch, 6, Boss(2500, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	TestTrue(TEXT("a push on the state topic applies"), Rig.PumpUntil([Object]() { return HealthOf(Object) == 2500; }));
	TestEqual(TEXT("and is no message"), Spy.Num(), 0);

	Rig.PushTopic(StructTopic, PushPayload(Rig, 1, TEXT("fist")));
	TestTrue(TEXT("a push on a message topic is a message"), Rig.PumpUntil([&Spy]() { return Spy.Num() == 1; }));
	TestEqual(TEXT("and changes no value"), HealthOf(Object), 2500);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageRoutesByTopicTest,
	"CrowdySDK.CrowdyExec.ServerMessage.PushIsRoutedByItsTopic", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageRoutesByTopicTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	FValuesSpy Values;
	Values.Watch(Object);
	const int32 Announced = Values.Num();

	FCrowdyNativeExecPush Push;
	Push.NodeType = TypeName;
	Push.Key = Rig.InstanceId;
	Push.Payload = PushPayload(Rig, 4, TEXT("bow"));
	Push.Topic = TEXT("no_such_topic");
	Object->HandlePushForTest(Push);
	TestTrue(TEXT("a topic the definition does not know is ignored"), Spy.Num() == 0 && Values.Num() == Announced);

	Push.Topic = ListTopic;
	Object->HandlePushForTest(Push);
	TestTrue(TEXT("and so is a message nothing listens to"), Spy.Num() == 0);

	Push.Topic = StructTopic;
	Object->HandlePushForTest(Push);
	TestTrue(TEXT("a message topic is announced and never read as the watched values"), Spy.Num() == 1 && Values.Num() == Announced && HealthOf(Object) == 3000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageBadPayloadTest,
	"CrowdySDK.CrowdyExec.ServerMessage.PayloadThatDoesNotDecodeIsDropped", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageBadPayloadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	AddExpectedMessagePlain(TEXT("does not fit its payload"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	if (!Rig.AckTopic(StructTopic))
	{
		return false;
	}
	Rig.PushTopic(StructTopic, {0x01});
	Rig.PumpFor(0.2);
	TestEqual(TEXT("a payload that is not the message's struct is not announced"), Spy.Num(), 0);
	TestTrue(TEXT("and leaves the object Ready"), Object->GetStatus() == ECrowdyServerObjectStatus::Ready);

	Rig.PushTopic(StructTopic, PushPayload(Rig, 3, TEXT("club")));
	TestTrue(TEXT("the next good message is heard"), Rig.PumpUntil([&Spy]() { return Spy.Num() == 1; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageBeforeReadyTest,
	"CrowdySDK.CrowdyExec.ServerMessage.MessageBeforeReadyWaitsForTheFirstRead", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageBeforeReadyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	if (!Rig.Open() || !Rig.AckTopic(StructTopic))
	{
		return false;
	}
	TestTrue(TEXT("the object is not Ready: its first read is not answered"), Object->GetStatus() == ECrowdyServerObjectStatus::Connecting);

	Rig.PushTopic(StructTopic, PushPayload(Rig, 9, TEXT("early")));
	Rig.PumpFor(0.2);
	TestEqual(TEXT("a message before the object is Ready is not announced"), Spy.Num(), 0);

	if (!Rig.AnswerFirstRead(Object, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TestTrue(TEXT("it is held until the first read is done, then announced"), Spy.Num() == 1 && IsPayload((*Spy.Heard)[0].Value, 9, TEXT("early")));
	Rig.PushTopic(StructTopic, PushPayload(Rig, 10, TEXT("late")));
	if (!TestTrue(TEXT("a message once Ready is announced at once, after it"), Rig.PumpUntil([&Spy]() { return Spy.Num() == 2; })))
	{
		return false;
	}
	TestTrue(TEXT("and it is the later one"), IsPayload((*Spy.Heard)[1].Value, 10, TEXT("late")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageRefusedSubscribeTest,
	"CrowdySDK.CrowdyExec.ServerMessage.RefusedSubscribeWarnsOnceAndLeavesTheObjectReady", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageRefusedSubscribeTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	AddExpectedMessagePlain(TEXT("could not listen to its BossHit message"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	FSentFrame Subscribe;
	if (!TestTrue(TEXT("the topic is subscribed"), Rig.WaitForFrame(KindSubscribe, StructTopic, Subscribe)))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("not client-callable")));
	Rig.PumpFor(0.2);
	TestTrue(TEXT("a refusal leaves the object Ready"), Object->GetStatus() == ECrowdyServerObjectStatus::Ready);
	TestTrue(TEXT("with no failure to report"), Object->GetFailureReason().IsEmpty());
	Rig.Tick(0.8f, 0.1f);
	TestFalse(TEXT("it is not asked again before the 1 s wait is over"), Rig.SentAny(KindSubscribe, StructTopic));
	Rig.Tick(0.4f, 0.1f);
	if (!TestTrue(TEXT("it is asked again after it, on the same connection and with no refresh"), Rig.WaitForFrame(KindSubscribe, StructTopic, Subscribe)))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Denied, Utf8Bytes(TEXT("not client-callable")));
	Rig.PumpFor(0.2);
	Rig.Tick(1.5f, 0.1f);
	TestFalse(TEXT("a second refusal waits longer, 2 s"), Rig.SentAny(KindSubscribe, StructTopic));
	Rig.Tick(0.7f, 0.1f);
	if (!TestTrue(TEXT("and is asked again after that"), Rig.WaitForFrame(KindSubscribe, StructTopic, Subscribe)))
	{
		return false;
	}
	Rig.Reply(Subscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.PumpFor(0.2);
	Rig.Tick(40.f, 0.5f);
	TestFalse(TEXT("an accepted subscribe is not asked again"), Rig.SentAny(KindSubscribe, StructTopic));
	TestTrue(TEXT("and a refusal was never a failure of the object"), Object->GetStatus() == ECrowdyServerObjectStatus::Ready);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageWaitsForReadTest,
	"CrowdySDK.CrowdyExec.ServerMessage.MessageDuringAReadSeesTheStateItDescribes", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageWaitsForReadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TSharedRef<TArray<int32>> HealthHeard = MakeShared<TArray<int32>>();
	Object->WatchMessage(StructName, FOnCrowdyServerMessage::FDelegate::CreateLambda([HealthHeard, Object](UCrowdyServerObject*, FName, const FInstancedStruct&)
	{
		HealthHeard->Add(HealthOf(Object));
	}));
	if (!Rig.AckTopic(StructTopic))
	{
		return false;
	}

	// A push that skips a sequence number makes the object read again.
	Rig.Push(Rig.PushMessage(Epoch, 8, Boss(2800, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	FSentFrame Read;
	if (!TestTrue(TEXT("a gap in the pushes asks for a read"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	Rig.Push(Rig.PushMessage(Epoch, 10, Boss(1500, false, ECrowdyExecTestPhase::Calm), HealthOnly));
	Rig.PushTopic(StructTopic, PushPayload(Rig, 2, TEXT("during")));
	Rig.PumpFor(0.2);
	TestEqual(TEXT("a message that arrives while the read is out is held back"), HealthHeard->Num(), 0);

	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::Ok, Rig.ReadMessage(9, Boss(2000, false, ECrowdyExecTestPhase::Calm)));
	if (!TestTrue(TEXT("and announced once the read is done"), Rig.PumpUntil([&HealthHeard]() { return HealthHeard->Num() >= 1; })))
	{
		return false;
	}
	TestEqual(TEXT("exactly once"), HealthHeard->Num(), 1);
	TestEqual(TEXT("after the push it overtook was applied, so the listener sees the new state"), (*HealthHeard)[0], 1500);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageReleaseStopsAnnouncementTest,
	"CrowdySDK.CrowdyExec.ServerMessage.ListenerThatReleasesTheObjectStopsTheAnnouncement", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageReleaseStopsAnnouncementTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TSharedRef<int32> Releases = MakeShared<int32>(0);
	FRig* RigPtr = &Rig;
	FMessageSpy After;
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Blueprint = MessageListener();
	Object->WatchMessage(StructName, FOnCrowdyServerMessage::FDelegate::CreateLambda([Releases, RigPtr](UCrowdyServerObject*, FName, const FInstancedStruct&)
	{
		++*Releases;
		RigPtr->Deinitialize();
	}));
	Object->WatchMessage(StructName, After.Listener());
	Object->K2_WatchMessage(StructName, EventOf(Blueprint.Get()));
	if (!Rig.AckTopic(StructTopic))
	{
		return false;
	}
	Rig.PushTopic(StructTopic, PushPayload(Rig, 1, TEXT("last")));
	if (!TestTrue(TEXT("the first listener runs and gives the object back"), Rig.PumpUntil([&Releases]() { return *Releases >= 1; })))
	{
		return false;
	}
	Rig.PumpFor(0.1);
	TestTrue(TEXT("the object is released"), Object->GetStatus() == ECrowdyServerObjectStatus::Released);
	TestTrue(TEXT("and neither the next C++ listener nor the Blueprint watcher hears the message"), After.Num() == 0 && Blueprint->EventPayloads.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageDeadWatcherUnsubscribesTest,
	"CrowdySDK.CrowdyExec.ServerMessage.DeadWatcherPrunedAtAMessageUnsubscribesTheTopic", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageDeadWatcherUnsubscribesTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Dead = MessageListener();
	Object->K2_WatchMessage(StructName, EventOf(Dead.Get()));
	if (!Rig.AckTopic(StructTopic))
	{
		return false;
	}
	Dead->MarkAsGarbage();
	Dead.Reset();

	Rig.PushTopic(StructTopic, PushPayload(Rig, 1, TEXT("nobody")));
	FSentFrame Unsubscribe;
	TestTrue(TEXT("the message drops the watcher whose object is gone and, with none left, unsubscribes the topic"),
		Rig.WaitForFrame(KindUnsubscribe, StructTopic, Unsubscribe));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageBindingsDestructorTest,
	"CrowdySDK.CrowdyExec.ServerMessage.BindingsGoneUnsubscribeTheTopic", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageBindingsDestructorTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Handler = MessageListener();
	{
		FCrowdyServerMessageBindings Held;
		Held.Follow(Object, FOnCrowdyServerMessage::FDelegate::CreateLambda([](UCrowdyServerObject*, FName, const FInstancedStruct&) {}));
		Held.Bind(StructName, Handler.Get(), BoundFunction);
		if (!Rig.AckTopic(StructTopic))
		{
			return false;
		}
	}
	FSentFrame Unsubscribe;
	TestTrue(TEXT("bindings that go out of scope stop listening, so the topic is unsubscribed"), Rig.WaitForFrame(KindUnsubscribe, StructTopic, Unsubscribe));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageServerOnlyTest,
	"CrowdySDK.CrowdyExec.ServerMessage.MessageWithoutPlayersCanListenIsNotSubscribed", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageServerOnlyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	AddExpectedMessagePlain(TEXT("PhaseShift is heard only by other Server Object types; turn on Players Can Listen to hear it in a game"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	Rig.Definition->Messages[1].bPlayersCanListen = false;
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	Rig.Sent.Reset();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Blueprint = MessageListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Bound = MessageListener();
	FMessageSpy Spy;
	TestFalse(TEXT("a message players cannot listen to gives no handle"), Object->WatchMessage(ListName, Spy.Listener()).IsValid());
	Object->K2_WatchMessage(ListName, EventOf(Blueprint.Get()));
	UCrowdyServerObjectLibrary::BindServerMessage(Object, Rig.Definition.Get(), ListName, Bound.Get(), BoundFunction);
	TestFalse(TEXT("and subscribes nothing"), Rig.SentAny(KindSubscribe, ListTopic));
	TestEqual(TEXT("not even once"), static_cast<int32>(Object->NumMessageSubscribesForTest()), 0);

	Object->WatchMessage(StructName, Spy.Listener());
	FSentFrame Subscribe;
	TestTrue(TEXT("while one players can listen to still subscribes"), Rig.WaitForFrame(KindSubscribe, StructTopic, Subscribe));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageRestartTest,
	"CrowdySDK.CrowdyExec.ServerMessage.RestartAfterFailingSubscribesTheTopicsAgain", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageRestartTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	FSentFrame StateSubscribe;
	FSentFrame Read;
	if (!Rig.Open() || !Rig.AckTopic(StructTopic) || !TestTrue(TEXT("the object subscribes the watched values"), Rig.WaitForFrame(KindSubscribe, TEXT("state"), StateSubscribe))
		|| !TestTrue(TEXT("and reads them"), Rig.WaitForFrame(KindCall, TEXT("read"), Read)))
	{
		return false;
	}
	Rig.Reply(StateSubscribe.Rid, ECrowdyNativeExecStatus::Ok, {});
	Rig.Reply(Read.Rid, ECrowdyNativeExecStatus::BadRequest, Utf8Bytes(TEXT("bad read")));
	if (!TestTrue(TEXT("a read the server calls bad fails the object"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Failed; })))
	{
		return false;
	}
	FSentFrame Unsubscribe;
	TestTrue(TEXT("which unsubscribes its message topic"), Rig.WaitForFrame(KindUnsubscribe, StructTopic, Unsubscribe));
	Rig.Sent.Reset();

	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Again = MakeOwner();
	TestTrue(TEXT("acquiring it again restarts it"), Rig.Acquire(Rig.InstanceId, Again.Get()) == Object);
	FSentFrame Resubscribe;
	TestTrue(TEXT("and its listeners' topic is subscribed again"), Rig.WaitForFrame(KindSubscribe, StructTopic, Resubscribe));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageRedialTest,
	"CrowdySDK.CrowdyExec.ServerMessage.RedialOnANewConnectionSubscribesTheTopicsAgain", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageRedialTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid())
	{
		return false;
	}
	// With no watched values the object holds no state subscription, so only the redial path can bring its topics back.
	Rig.Definition->WatchedFields.Reset();
	if (!AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = Rig.Acquire(Rig.InstanceId, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	if (!Rig.Open() || !TestTrue(TEXT("an object with no watched values is Ready at once"), Rig.PumpUntil([Object]() { return Object->GetStatus() == ECrowdyServerObjectStatus::Ready; }))
		|| !Rig.AckTopic(StructTopic))
	{
		return false;
	}

	Rig.Subsystem->DropConnectionForTest();
	Rig.Sent.Reset();
	Rig.Tick(2.f, 0.1f);
	if (!TestTrue(TEXT("the subsystem dials a new connection"), Rig.PumpUntil([&Rig]() { return Rig.Client->NumTestExecConnections() >= 2; }, 6.0)))
	{
		return false;
	}
	Rig.Client->TestExecOpen();
	FSentFrame Subscribe;
	if (TestTrue(TEXT("and subscribes the topic on it"), Rig.WaitForFrame(KindSubscribe, StructTopic, Subscribe, 5.0)))
	{
		TestTrue(TEXT("on this instance"), Rig.IsAddressed(Subscribe));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageBindingChangesDuringNotifyTest,
	"CrowdySDK.CrowdyExec.ServerMessage.HandlerThatUnbindsAndBindsDuringAMessage", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageBindingChangesDuringNotifyTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	UCrowdyServerObjectDefinition* Definition = Rig.Definition.Get();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> First = MessageListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Second = MessageListener();
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Third = MessageListener();
	UCrowdyServerObjectLibrary::BindServerMessage(Object, Definition, StructName, First.Get(), BoundFunction);
	UCrowdyServerObjectLibrary::BindServerMessage(Object, Definition, StructName, Second.Get(), BoundFunction);
	UCrowdyServerObjectTestListener* Dropped = Second.Get();
	UCrowdyServerObjectTestListener* Added = Third.Get();
	First->OnMessage = [Object, Definition, Dropped, Added]()
	{
		UCrowdyServerObjectLibrary::UnbindServerMessage(Object, Definition, StructName, Dropped, BoundFunction);
		UCrowdyServerObjectLibrary::BindServerMessage(Object, Definition, StructName, Added, BoundFunction);
	};
	if (!Rig.AckTopic(StructTopic))
	{
		return false;
	}
	Rig.PushTopic(StructTopic, PushPayload(Rig, 1, TEXT("one")));
	if (!TestTrue(TEXT("the first handler runs"), Rig.PumpUntil([&First]() { return First->MessagePayloads.Num() >= 1; })))
	{
		return false;
	}
	Rig.PumpFor(0.1);
	First->OnMessage = nullptr;
	TestEqual(TEXT("a handler it unbound is skipped for the message under way"), Second->MessagePayloads.Num(), 0);
	TestEqual(TEXT("and one it bound hears only the next"), Third->MessagePayloads.Num(), 0);

	Rig.PushTopic(StructTopic, PushPayload(Rig, 2, TEXT("two")));
	TestTrue(TEXT("the next message reaches the handlers now bound"), Rig.PumpUntil([&Third]() { return Third->MessagePayloads.Num() >= 1; }));
	Rig.PumpFor(0.1);
	TestTrue(TEXT("the first and the added, and not the unbound"), First->MessagePayloads.Num() == 2 && Third->MessagePayloads.Num() == 1 && Second->MessagePayloads.Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageUnknownTest,
	"CrowdySDK.CrowdyExec.ServerMessage.WatchingAMessageTheDefinitionLacksWarns", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageUnknownTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	AddExpectedMessagePlain(TEXT("has no message named Nope"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	TestFalse(TEXT("an unknown message gives no handle"), Object->WatchMessage(TEXT("Nope"), Spy.Listener()).IsValid());
	Rig.PumpFor(0.15);
	TestTrue(TEXT("and subscribes nothing"), Rig.Sent.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageReleaseTest,
	"CrowdySDK.CrowdyExec.ServerMessage.GivingTheObjectBackDropsItsSubscriptions", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageReleaseTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObject> Object(ReadyBoss(Rig, Owner.Get()));
	if (!Object)
	{
		return false;
	}
	FMessageSpy Spy;
	Object->WatchMessage(StructName, Spy.Listener());
	if (!Rig.AckTopic(StructTopic))
	{
		return false;
	}
	Destroy(Owner);
	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds + 1.f);
	TestTrue(TEXT("the object is given back"), Object->GetStatus() == ECrowdyServerObjectStatus::Released);
	FSentFrame Unsubscribe;
	TestTrue(TEXT("and its message topic is unsubscribed"), Rig.WaitForFrame(KindUnsubscribe, StructTopic, Unsubscribe));
	TestFalse(TEXT("a released object accepts no new listener"), Object->WatchMessage(StructName, Spy.Listener()).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageComponentRejoinTest,
	"CrowdySDK.CrowdyExec.ServerMessage.ComponentRejoinMovesTheInterest", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageComponentRejoinTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectComponent> Component(NewObject<UCrowdyServerObjectComponent>(GetTransientPackage(), NAME_None, RF_Transient));
	Component->Definition = Rig.Definition.Get();
	Component->InstanceMode = ECrowdyServerObjectInstanceMode::InstanceId;
	Component->InstanceId = TEXT("boss-1");
	FCrowdyServerObjectMessageTestAccess::UseServerObjects(*Component, Rig.Subsystem.Get());
	FCrowdyServerObjectMessageTestAccess::Join(*Component);
	UCrowdyServerObject* First = Component->GetServerObject();
	if (!TestNotNull(TEXT("the component joins boss-1"), First) || !Rig.Open()
		|| !Rig.AnswerFirstRead(First, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Handler = MessageListener();
	UCrowdyServerObjectLibrary::BindServerMessage(Component.Get(), Rig.Definition.Get(), StructName, Handler.Get(), BoundFunction);
	FSentFrame Subscribed;
	if (!TestTrue(TEXT("binding a message on the component subscribes it on the object held"), Rig.WaitForFrame(KindSubscribe, StructTopic, Subscribed))
		|| !TestTrue(TEXT("on boss-1"), Rig.IsAddressed(Subscribed)))
	{
		return false;
	}
	Rig.Reply(Subscribed.Rid, ECrowdyNativeExecStatus::Ok, {});

	Component->InstanceId = TEXT("boss-2");
	FCrowdyServerObjectMessageTestAccess::Join(*Component);
	UCrowdyServerObject* Second = Component->GetServerObject();
	if (!TestTrue(TEXT("rejoining moves the component to boss-2"), Second && Second != First))
	{
		return false;
	}
	FSentFrame Left;
	FSentFrame Moved;
	if (!TestTrue(TEXT("the interest leaves boss-1"), Rig.WaitForFrame(KindUnsubscribe, StructTopic, Left)) || !TestTrue(TEXT("on boss-1"), Rig.IsAddressed(Left)))
	{
		return false;
	}
	Rig.InstanceId = TEXT("boss-2");
	if (!TestTrue(TEXT("and arrives on boss-2"), Rig.WaitForFrame(KindSubscribe, StructTopic, Moved)) || !TestTrue(TEXT("addressed to it"), Rig.IsAddressed(Moved)))
	{
		return false;
	}
	Rig.Reply(Moved.Rid, ECrowdyNativeExecStatus::Ok, {});
	if (!Rig.AnswerFirstRead(Second, Boss(10, false, ECrowdyExecTestPhase::Calm), 1))
	{
		return false;
	}
	Rig.PushTopic(StructTopic, PushPayload(Rig, 11, TEXT("moved")));
	TestTrue(TEXT("a message of the new object reaches the handler"), Rig.PumpUntil([&Handler]() { return Handler->MessagePayloads.Num() == 1; }));
	TestTrue(TEXT("with its payload"), Handler->MessagePayloads.Num() == 1 && IsPayload(Handler->MessagePayloads[0], 11, TEXT("moved")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageLinkKeptTest,
	"CrowdySDK.CrowdyExec.ServerMessage.LinkWithOnlyAMessageBindingIsKept", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageLinkKeptTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	UCrowdyServerObjectDefinition* Definition = Rig.Definition.Get();
	TStrongObjectPtr<UCrowdyServerObjectLink> Link(Rig.Subsystem->FindLink(Widget.Get(), Definition, ECrowdyServerObjectFind::InstanceId, Rig.InstanceId, 0));
	UCrowdyServerObject* Object = Link ? Link->GetServerObject() : nullptr;
	if (!Object || !Rig.Open() || !Rig.AnswerFirstRead(Object, Boss(3000, false, ECrowdyExecTestPhase::Calm), 5))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectLink> Idle(Rig.Subsystem->FindLink(Widget.Get(), Definition, ECrowdyServerObjectFind::InstanceId, TEXT("boss-9"), 0));
	if (!Idle)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Handler = MessageListener();
	UCrowdyServerObjectLibrary::BindServerMessage(Link.Get(), Definition, StructName, Handler.Get(), BoundFunction);
	if (!Rig.AckTopic(StructTopic))
	{
		return false;
	}

	Rig.Tick(UCrowdyServerObjectSubsystem::GracePeriodSeconds * 3.f);
	TestTrue(TEXT("a Link only a message binding uses is kept past the grace period"), Link->GetServerObject() == Object && Object->GetStatus() == ECrowdyServerObjectStatus::Ready);
	TestNull(TEXT("while a Link with no handler is let go"), Idle->GetServerObject());
	Rig.PushTopic(StructTopic, PushPayload(Rig, 5, TEXT("link")));
	TestTrue(TEXT("and it still hears the object's messages"), Rig.PumpUntil([&Handler]() { return Handler->MessagePayloads.Num() == 1; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyServerMessageLibraryBindingTest,
	"CrowdySDK.CrowdyExec.ServerMessage.BindingMovesWithTheHandlerAndUnbinds", CrowdyServerObjectTests::TestFlags)
bool FCrowdyServerMessageLibraryBindingTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyServerObjectTests;
	using namespace CrowdyServerObjectMessageTests;
	FRig Rig(*this);
	if (!Rig.IsValid() || !AddMessages(Rig))
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Owner = MakeOwner();
	TStrongObjectPtr<UCrowdyServerObjectTestOwner> Widget = MakeOwner();
	UCrowdyServerObject* Object = ReadyBoss(Rig, Owner.Get());
	UCrowdyServerObjectDefinition* Definition = Rig.Definition.Get();
	TStrongObjectPtr<UCrowdyServerObjectLink> Link(Rig.Subsystem->FindLink(Widget.Get(), Definition, ECrowdyServerObjectFind::InstanceId, Rig.InstanceId, 0));
	if (!Object || !Link)
	{
		return false;
	}
	TStrongObjectPtr<UCrowdyServerObjectTestListener> Handler = MessageListener();
	UCrowdyServerObjectLibrary::BindServerMessage(Object, Definition, StructName, Handler.Get(), BoundFunction);
	TestEqual(TEXT("the object holds the binding"), Object->GetMessageBindings().Num(), 1);
	UCrowdyServerObjectLibrary::BindServerMessage(Link.Get(), Definition, StructName, Handler.Get(), BoundFunction);
	TestTrue(TEXT("binding the same handler on a Link moves it"), Object->GetMessageBindings().Num() == 0 && Link->GetMessageBindings().Num() == 1);
	AckEvery(Rig, StructTopic);
	Rig.PushTopic(StructTopic, PushPayload(Rig, 6, TEXT("once")));
	TestTrue(TEXT("a message runs the handler"), Rig.PumpUntil([&Handler]() { return Handler->MessagePayloads.Num() >= 1; }));
	Rig.PumpFor(0.1);
	TestEqual(TEXT("once"), Handler->MessagePayloads.Num(), 1);

	UCrowdyServerObjectLibrary::UnbindServerMessage(Link.Get(), Definition, StructName, Handler.Get(), BoundFunction);
	TestEqual(TEXT("unbinding drops it"), Link->GetMessageBindings().Num(), 0);
	FSentFrame Unsubscribe;
	TestTrue(TEXT("and the topic is unsubscribed with nothing left to hear it"), Rig.WaitForFrame(KindUnsubscribe, StructTopic, Unsubscribe));
	return true;
}

#endif
