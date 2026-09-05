#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Subscription/FCrowdyPayloadKey.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyModelChangedPing.h"
#include "StructUtils/InstancedStruct.h"
#include "Utils/UEventPayloadRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyGameModelInboundTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Carries the fallback model-changed ping under its registered event type number, which is how the
	// router matches it to the Game Model subsystem's subscription.
	struct FCrowdyModelChangedPingMessage : ICrowdyMessage
	{
		FInstancedStruct EventPayload;
		FCrowdyTypeID TypeID = 0;

		virtual ECrowdyMessageType GetType() const override { return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION; }
		virtual FName GetTypeName() const override { return TEXT("Model Changed Ping Test Message"); }
		virtual TArray<uint8> Serialize() const override { return {}; }
		[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame&) override { return true; }
		virtual FCrowdyPayloadKey GetPayloadKey() const override { return FCrowdyPayloadKey::Event(TypeID); }
		virtual const FInstancedStruct* GetPayload() const override
		{
			return EventPayload.IsValid() ? &EventPayload : nullptr;
		}
	};
}

// A model-changed notification reaches the notification sink during the dispatch that delivered it, so an
// observer sees the change on the same frame the message arrived rather than one frame later.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelNotificationDuringDispatchTest,
	"CrowdySDK.Inbound.ModelChangedNotifiesDuringDispatch", CrowdyGameModelInboundTestFlags)
bool FCrowdyGameModelNotificationDuringDispatchTest::RunTest(const FString& Parameters)
{
	FCrowdyServiceRegistry Registry;

	UCrowdyGameModelSubsystem* GameModel = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	GameModel->BeginWorldSessionForTest();
	GameModel->SubscribeToModelChangedCarriersForTest(Registry);

	int32 NotifyCount = 0;
	FString NotifiedContainerId;
	GameModel->OnModelChanged().AddLambda(
		[&NotifyCount, &NotifiedContainerId](const FCrowdyModelChangeHint& Hint)
		{
			++NotifyCount;
			NotifiedContainerId = Hint.ContainerId;
		});

	FCrowdyModelChangedPing Ping;
	Ping.EntityID = FGuid::NewGuid();
	Ping.ContainerId = TEXT("container-1");

	TSharedRef<FCrowdyModelChangedPingMessage, ESPMode::ThreadSafe> Message =
		MakeShared<FCrowdyModelChangedPingMessage, ESPMode::ThreadSafe>();
	Message->EventPayload = FInstancedStruct::Make(Ping);
	UEventPayloadRegistry::Get()->GetID(FCrowdyModelChangedPing::StaticStruct(), Message->TypeID);

	Registry.DispatchMessage(Message);

	TestEqual(TEXT("the sink was notified before DispatchMessage returned"), NotifyCount, 1);
	TestEqual(TEXT("the notification carried the ping's container id"), NotifiedContainerId, Ping.ContainerId);
	return true;
}

// The registry belongs to the game instance and outlives every world under it, so each level travelled through left
// its subsystem subscribed and every frame was handled once per subscription: a signal fired once per level the
// player had been through, and every re-pull, ensure and bind it drove was multiplied the same way. Exactly one
// subsystem may hold the claim per registry, and it is the one that subscribed most recently, since subscription
// order follows travel order. Decided by that order alone, with no dependence on what a departed world reports
// about itself, because a departed world here still described itself as the game instance's current world.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelOneSubscriberPerRegistryTest,
	"CrowdySDK.Inbound.OneModelSubscriberPerRegistry", CrowdyGameModelInboundTestFlags)
bool FCrowdyGameModelOneSubscriberPerRegistryTest::RunTest(const FString& Parameters)
{
	FCrowdyServiceRegistry Registry;

	// Three subsystems subscribing in travel order against ONE registry, as three levels in a row produce.
	int32 FirstCount = 0, SecondCount = 0, ThirdCount = 0;
	auto MakeSubscriber = [&Registry](int32& Counter) -> UCrowdyGameModelSubsystem*
	{
		UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
		Model->BeginWorldSessionForTest();
		Model->SubscribeToModelChangedCarriersForTest(Registry);
		Model->OnModelChanged().AddLambda([&Counter](const FCrowdyModelChangeHint&) { ++Counter; });
		return Model;
	};

	UCrowdyGameModelSubsystem* First = MakeSubscriber(FirstCount);
	UCrowdyGameModelSubsystem* Second = MakeSubscriber(SecondCount);
	UCrowdyGameModelSubsystem* Third = MakeSubscriber(ThirdCount);
	(void)First; (void)Second; (void)Third;

	FCrowdyModelChangedPing Ping;
	Ping.EntityID = FGuid::NewGuid();
	Ping.ContainerId = TEXT("container-1");

	TSharedRef<FCrowdyModelChangedPingMessage, ESPMode::ThreadSafe> Message =
		MakeShared<FCrowdyModelChangedPingMessage, ESPMode::ThreadSafe>();
	Message->EventPayload = FInstancedStruct::Make(Ping);
	UEventPayloadRegistry::Get()->GetID(FCrowdyModelChangedPing::StaticStruct(), Message->TypeID);

	Registry.DispatchMessage(Message);

	// One delivery, handled exactly once, by the world entered last.
	TestEqual(TEXT("the first world no longer handles deliveries"), FirstCount, 0);
	TestEqual(TEXT("nor does the second"), SecondCount, 0);
	TestEqual(TEXT("the most recently subscribed world handles it, exactly once"), ThirdCount, 1);

	// A departed world must never take the claim back. Its subsystem is still alive and the subscribe path is
	// reached from three call sites (including the entity-bind one), so without the latch a lingering world would
	// displace the world the player is actually in and silence it, which is worse than the duplication.
	First->SubscribeToModelChangedCarriersForTest(Registry);
	Registry.DispatchMessage(Message);

	TestEqual(TEXT("a superseded world does not take the subscription back"), FirstCount, 0);
	TestEqual(TEXT("and the current world keeps handling deliveries"), ThirdCount, 2);

	return true;
}

#endif
