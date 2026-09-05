// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Internal/FCrowdyRoutingTable.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/Channels/FChannelMessages.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Messages/GameObjects/FServerEventNotification.h"
#include "StructUtils/InstancedStruct.h"
#include "Utils/UEventPayloadRegistry.h"

namespace CrowdyRouterTest
{
	constexpr EAutomationTestFlags RouterTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	/** Declares no payload, so it routes through the opcode table. */
	struct FRouterOpcodeMessage : ICrowdyMessage
	{
		ECrowdyMessageType Opcode = ECrowdyMessageType::GENERIC_SPATIAL_1;

		explicit FRouterOpcodeMessage(const ECrowdyMessageType InOpcode) : Opcode(InOpcode) {}

		virtual ECrowdyMessageType GetType() const override { return Opcode; }
		virtual FName GetTypeName() const override { return TEXT("Router Test Opcode Message"); }
		virtual TArray<uint8> Serialize() const override { return {}; }
		[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame&) override { return true; }
	};

	/** Declares whatever payload key a case needs, so it routes through the payload table. */
	struct FRouterPayloadMessage : ICrowdyMessage
	{
		ECrowdyMessageType Opcode = ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION;
		FCrowdyPayloadKey Key;

		FRouterPayloadMessage(const ECrowdyMessageType InOpcode, const FCrowdyPayloadKey& InKey)
			: Opcode(InOpcode), Key(InKey) {}

		virtual ECrowdyMessageType GetType() const override { return Opcode; }
		virtual FName GetTypeName() const override { return TEXT("Router Test Payload Message"); }
		virtual TArray<uint8> Serialize() const override { return {}; }
		[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame&) override { return true; }
		virtual FCrowdyPayloadKey GetPayloadKey() const override { return Key; }
	};

	static TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> MakeOpcodeMessage(const ECrowdyMessageType Opcode)
	{
		return MakeShared<FRouterOpcodeMessage, ESPMode::ThreadSafe>(Opcode);
	}

	static TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> MakeEventMessage(const FCrowdyTypeID TypeID)
	{
		return MakeShared<FRouterPayloadMessage, ESPMode::ThreadSafe>(
			ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION, FCrowdyPayloadKey::Event(TypeID));
	}

	static TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> MakeActorUpdateMessage(const FCrowdyTypeID TypeID)
	{
		return MakeShared<FRouterPayloadMessage, ESPMode::ThreadSafe>(
			ECrowdyMessageType::ACTOR_UPDATE_NOTIFICATION, FCrowdyPayloadKey::ActorUpdate(TypeID));
	}

	static FCrowdySubscriptionOptions MakeOptions(const ECrowdySubscriptionRole Role, const TCHAR* Name,
		const bool bExclusive = false)
	{
		FCrowdySubscriptionOptions Options;
		Options.Role = Role;
		Options.SubscriberName = Name;
		Options.bRequiresExclusiveHandling = bExclusive;
		return Options;
	}
}

// A subscription taken from inside a running delivery joins the table that replaces the one being
// walked, so it starts receiving on the next message and never mid-fan-out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRouterSubscribeDuringDeliveryTest,
	"CrowdySDK.Net.Router.SubscribeDuringDeliveryTakesEffectNext", CrowdyRouterTest::RouterTestFlags)
bool FCrowdyRouterSubscribeDuringDeliveryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyRouterTest;

	FCrowdyServiceRegistry Registry;
	constexpr FCrowdyTypeID PayloadID = 4242;

	int32 FirstCount = 0;
	int32 SecondCount = 0;

	FCrowdySubscription Second;

	const FCrowdySubscription First = Registry.SubscribeToEventPayload(PayloadID,
		MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("First")),
		[&Registry, &FirstCount, &SecondCount, &Second](const FCrowdyDelivery&)
		{
			++FirstCount;
			if (!Second.IsValid())
			{
				Second = Registry.SubscribeToEventPayload(PayloadID,
					MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("Second")),
					[&SecondCount](const FCrowdyDelivery&) { ++SecondCount; });
			}
		});

	Registry.DispatchMessage(MakeEventMessage(PayloadID));
	TestEqual(TEXT("the first subscriber ran on the first message"), FirstCount, 1);
	TestEqual(TEXT("the subscriber added during that delivery did not run on it"), SecondCount, 0);

	Registry.DispatchMessage(MakeEventMessage(PayloadID));
	TestEqual(TEXT("the first subscriber ran again"), FirstCount, 2);
	TestEqual(TEXT("the added subscriber runs from the next message onward"), SecondCount, 1);

	return true;
}

// Releasing a handle before its turn in a fan-out stops it from being called on that same dispatch,
// because the liveness flag is checked right before every invocation in the delivery loop. This is
// a same-thread, sequential guarantee: it does not show that Release() interrupts a handler already
// running concurrently on another thread, which it does not.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRouterReleaseDuringDeliveryTest,
	"CrowdySDK.Net.Router.ReleaseDuringDeliveryStopsImmediately", CrowdyRouterTest::RouterTestFlags)
bool FCrowdyRouterReleaseDuringDeliveryTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyRouterTest;

	FCrowdyServiceRegistry Registry;
	constexpr FCrowdyTypeID PayloadID = 77;

	int32 ReleaserCount = 0;
	int32 VictimCount = 0;

	FCrowdySubscription Victim;

	// Subscribed first, so it is delivered to first and its release lands before the victim's turn.
	const FCrowdySubscription Releaser = Registry.SubscribeToEventPayload(PayloadID,
		MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("Releaser")),
		[&ReleaserCount, &Victim](const FCrowdyDelivery&)
		{
			++ReleaserCount;
			Victim.Release();
		});

	Victim = Registry.SubscribeToEventPayload(PayloadID,
		MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("Victim")),
		[&VictimCount](const FCrowdyDelivery&) { ++VictimCount; });

	Registry.DispatchMessage(MakeEventMessage(PayloadID));
	TestEqual(TEXT("the releasing subscriber ran"), ReleaserCount, 1);
	TestEqual(TEXT("the subscriber released mid-delivery was never called on that message"), VictimCount, 0);

	for (int32 Index = 0; Index < 100; ++Index)
	{
		Registry.DispatchMessage(MakeEventMessage(PayloadID));
	}

	TestEqual(TEXT("the released subscriber stays silent"), VictimCount, 0);

	return true;
}

// The three routing rules the old dispatch tables encoded, restated on the new router: opcodes match
// exactly with no fallback, a claimed event excludes the fallback but not other claimants, and actor
// updates are additive.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRouterRoutingEquivalenceTest,
	"CrowdySDK.Net.Router.RoutingEquivalence", CrowdyRouterTest::RouterTestFlags)
bool FCrowdyRouterRoutingEquivalenceTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyRouterTest;

	{
		FCrowdyServiceRegistry Registry;

		int32 PingA = 0;
		int32 PingB = 0;
		int32 Voxel = 0;
		int32 EventFallback = 0;

		const FCrowdySubscription SubA = Registry.SubscribeToOpcode(ECrowdyMessageType::GENERIC_SPATIAL_1,
			MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("PingA")),
			[&PingA](const FCrowdyDelivery&) { ++PingA; });

		const FCrowdySubscription SubB = Registry.SubscribeToOpcode(ECrowdyMessageType::GENERIC_SPATIAL_1,
			MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("PingB")),
			[&PingB](const FCrowdyDelivery&) { ++PingB; });

		const FCrowdySubscription SubVoxel = Registry.SubscribeToOpcode(ECrowdyMessageType::VOXEL_UPDATE_NOTIFICATION,
			MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("Voxel")),
			[&Voxel](const FCrowdyDelivery&) { ++Voxel; });

		const FCrowdySubscription SubFallback = Registry.SubscribeToAllPayloads(ECrowdyPayloadCategory::Event,
			MakeOptions(ECrowdySubscriptionRole::Fallback, TEXT("EventFallback")),
			[&EventFallback](const FCrowdyDelivery&) { ++EventFallback; });

		Registry.DispatchMessage(MakeOpcodeMessage(ECrowdyMessageType::GENERIC_SPATIAL_1));

		TestEqual(TEXT("the first opcode subscriber received it once"), PingA, 1);
		TestEqual(TEXT("the second opcode subscriber received it once"), PingB, 1);
		TestEqual(TEXT("a subscriber on another opcode received nothing"), Voxel, 0);
		TestEqual(TEXT("an event fallback does not see an opcode-routed message"), EventFallback, 0);
	}

	{
		FCrowdyServiceRegistry Registry;

		constexpr FCrowdyTypeID ClaimedID = 1234;
		constexpr FCrowdyTypeID UnclaimedID = 5678;

		int32 HandlerA = 0;
		int32 HandlerB = 0;
		int32 Fallback = 0;

		const FCrowdySubscription SubA = Registry.SubscribeToEventPayload(ClaimedID,
			MakeOptions(ECrowdySubscriptionRole::Handle, TEXT("HandlerA")),
			[&HandlerA](const FCrowdyDelivery&) { ++HandlerA; });

		const FCrowdySubscription SubB = Registry.SubscribeToEventPayload(ClaimedID,
			MakeOptions(ECrowdySubscriptionRole::Handle, TEXT("HandlerB")),
			[&HandlerB](const FCrowdyDelivery&) { ++HandlerB; });

		const FCrowdySubscription SubFallback = Registry.SubscribeToAllPayloads(ECrowdyPayloadCategory::Event,
			MakeOptions(ECrowdySubscriptionRole::Fallback, TEXT("Fallback")),
			[&Fallback](const FCrowdyDelivery&) { ++Fallback; });

		Registry.DispatchMessage(MakeEventMessage(ClaimedID));

		TestEqual(TEXT("the first handler received the claimed event"), HandlerA, 1);
		TestEqual(TEXT("the second handler received it too"), HandlerB, 1);
		TestEqual(TEXT("the fallback is excluded by a claimed event"), Fallback, 0);

		Registry.DispatchMessage(MakeEventMessage(UnclaimedID));

		TestEqual(TEXT("the fallback receives an unclaimed event"), Fallback, 1);
		TestEqual(TEXT("the first handler sees nothing it did not claim"), HandlerA, 1);
		TestEqual(TEXT("the second handler sees nothing it did not claim"), HandlerB, 1);
	}

	{
		FCrowdyServiceRegistry Registry;

		constexpr FCrowdyTypeID UpdateID = 4321;

		int32 Wildcard = 0;
		int32 Specific = 0;
		int32 BothKeysOneSubscriber = 0;

		const FCrowdySubscription SubWildcard = Registry.SubscribeToAllPayloads(ECrowdyPayloadCategory::ActorUpdate,
			MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("Tracker")),
			[&Wildcard](const FCrowdyDelivery&) { ++Wildcard; });

		const FCrowdySubscription SubSpecific = Registry.SubscribeToActorUpdatePayload(UpdateID,
			MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("Specific")),
			[&Specific](const FCrowdyDelivery&) { ++Specific; });

		Registry.DispatchMessage(MakeActorUpdateMessage(UpdateID));

		TestEqual(TEXT("the actor-update wildcard received it"), Wildcard, 1);
		TestEqual(TEXT("the specific actor-update subscriber received it too"), Specific, 1);

		// One subscription holding both a wildcard and a specific key over a SINGLE handle and a
		// SINGLE callback is called once per key it holds, not once per message. The tables this
		// replaces collapsed the two into a single call; no shipped consumer holds both, so the
		// difference is documented rather than hidden.
		const FCrowdySubscriptionKey DualKeys[] =
		{
			FCrowdySubscriptionKey::AllPayloads(ECrowdyPayloadCategory::ActorUpdate),
			FCrowdySubscriptionKey::ActorUpdatePayload(UpdateID),
		};

		const FCrowdySubscription Dual = Registry.Subscribe(DualKeys,
			MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("Dual")),
			[&BothKeysOneSubscriber](const FCrowdyDelivery&) { ++BothKeysOneSubscriber; });

		Registry.DispatchMessage(MakeActorUpdateMessage(UpdateID));

		TestEqual(TEXT("one subscription holding both keys is called once per key on a single dispatch"),
			BothKeysOneSubscriber, 2);
	}

	return true;
}

// A second handler on a key whose first handler declared it needs to be alone is reported, naming
// both subscribers and the key, and both keep receiving so the report does not change behaviour.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRouterExclusiveConflictTest,
	"CrowdySDK.Net.Router.ExclusiveConflictIsReported", CrowdyRouterTest::RouterTestFlags)
bool FCrowdyRouterExclusiveConflictTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyRouterTest;

	constexpr FCrowdyTypeID PayloadID = 9001;

	AddExpectedErrorPlain(TEXT("[Router] 'Alpha' and 'Beta' both handle Event/9001"),
		EAutomationExpectedErrorFlags::Contains, 1);

	FCrowdyServiceRegistry Registry;

	int32 AlphaCount = 0;
	int32 BetaCount = 0;

	const FCrowdySubscription Alpha = Registry.SubscribeToEventPayload(PayloadID,
		MakeOptions(ECrowdySubscriptionRole::Handle, TEXT("Alpha"), true),
		[&AlphaCount](const FCrowdyDelivery&) { ++AlphaCount; });

	const FCrowdySubscription Beta = Registry.SubscribeToEventPayload(PayloadID,
		MakeOptions(ECrowdySubscriptionRole::Handle, TEXT("Beta")),
		[&BetaCount](const FCrowdyDelivery&) { ++BetaCount; });

	const TArray<FCrowdyRoutingConflict> Conflicts = Registry.GetConflicts();
	TestEqual(TEXT("exactly one conflict is recorded"), Conflicts.Num(), 1);

	if (Conflicts.Num() == 1)
	{
		const FString Text = Conflicts[0].Describe();
		TestTrue(TEXT("the conflict names the first subscriber"), Text.Contains(TEXT("Alpha")));
		TestTrue(TEXT("the conflict names the second subscriber"), Text.Contains(TEXT("Beta")));
		TestTrue(TEXT("the conflict names the key"),
			Text.Contains(FCrowdyPayloadKey::Event(PayloadID).Describe()));
	}

	Registry.DispatchMessage(MakeEventMessage(PayloadID));

	TestEqual(TEXT("the first handler is still delivered to"), AlphaCount, 1);
	TestEqual(TEXT("the second handler is still delivered to"), BetaCount, 1);

	return true;
}

// A payload nobody subscribes to is counted every time and reported once, and adding a fallback
// stops it being counted at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRouterUnroutedPayloadTest,
	"CrowdySDK.Net.Router.UnroutedPayloadIsReported", CrowdyRouterTest::RouterTestFlags)
bool FCrowdyRouterUnroutedPayloadTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyRouterTest;

	constexpr FCrowdyTypeID PayloadID = 4242;

	AddExpectedMessagePlain(TEXT("[Router] nothing subscribes to Event/4242"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

	FCrowdyServiceRegistry Registry;

	Registry.DispatchMessage(MakeEventMessage(PayloadID));

	FCrowdyRoutingStats Stats = Registry.GetStats();
	TestEqual(TEXT("the unrouted payload is counted"), static_cast<int32>(Stats.UnroutedCount), 1);
	TestEqual(TEXT("the key is listed once"), Stats.UnroutedKeys.Num(), 1);

	if (Stats.UnroutedKeys.Num() == 1)
	{
		TestTrue(TEXT("the listed key names the category and the id"),
			Stats.UnroutedKeys[0].Contains(TEXT("Event/4242")));
	}

	Registry.DispatchMessage(MakeEventMessage(PayloadID));

	Stats = Registry.GetStats();
	TestEqual(TEXT("the second arrival is counted too"), static_cast<int32>(Stats.UnroutedCount), 2);
	TestEqual(TEXT("but the key is still listed only once"), Stats.UnroutedKeys.Num(), 1);

	int32 FallbackCount = 0;
	const FCrowdySubscription Fallback = Registry.SubscribeToAllPayloads(ECrowdyPayloadCategory::Event,
		MakeOptions(ECrowdySubscriptionRole::Fallback, TEXT("Fallback")),
		[&FallbackCount](const FCrowdyDelivery&) { ++FallbackCount; });

	Registry.DispatchMessage(MakeEventMessage(PayloadID));

	Stats = Registry.GetStats();
	TestEqual(TEXT("the fallback received it"), FallbackCount, 1);
	TestEqual(TEXT("a routed payload does not raise the unrouted count"), static_cast<int32>(Stats.UnroutedCount), 2);

	return true;
}

// One logical notification arrives on three unrelated carriers. One handle over the three keys gives
// one callback, and the opcode on the delivery still tells them apart.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRouterThreeCarriersTest,
	"CrowdySDK.Net.Router.GameModelThreeCarriersOneSubscription", CrowdyRouterTest::RouterTestFlags)
bool FCrowdyRouterThreeCarriersTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyRouterTest;

	FCrowdyServiceRegistry Registry;

	constexpr FCrowdyTypeID ServerEventID = 60000;

	const FCrowdySubscriptionKey StructKey =
		FCrowdySubscriptionKey::EventPayload(FCrowdyEntitySpawnEvent::StaticStruct());

	FCrowdyTypeID StructEventID = CROWDY_INVALID_TYPE_ID;
	TestTrue(TEXT("the struct carrier resolves to a payload id"),
		UEventPayloadRegistry::Get()->GetID(FCrowdyEntitySpawnEvent::StaticStruct(), StructEventID));

	TestNotEqual(TEXT("the two event carriers use distinct payload ids"),
		static_cast<int32>(StructEventID), static_cast<int32>(ServerEventID));

	const FCrowdySubscriptionKey Carriers[] =
	{
		FCrowdySubscriptionKey::EventPayload(ServerEventID),
		StructKey,
		FCrowdySubscriptionKey::Opcode(ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION),
	};

	int32 Calls = 0;
	TArray<ECrowdyMessageType> SeenOpcodes;

	FCrowdySubscription Subscription = Registry.Subscribe(Carriers,
		MakeOptions(ECrowdySubscriptionRole::Handle, TEXT("GameModel")),
		[&Calls, &SeenOpcodes](const FCrowdyDelivery& Delivery)
		{
			++Calls;
			SeenOpcodes.Add(Delivery.Opcode);
		});

	const TSharedRef<FServerEventNotification, ESPMode::ThreadSafe> ServerEvent =
		MakeShared<FServerEventNotification, ESPMode::ThreadSafe>();
	ServerEvent->EventType = ServerEventID;

	const TSharedRef<FGameEventNotification, ESPMode::ThreadSafe> ClientEvent =
		MakeShared<FGameEventNotification, ESPMode::ThreadSafe>();
	ClientEvent->EventType = StructEventID;
	ClientEvent->State.InitializeAs<FCrowdyEntitySpawnEvent>();

	const TSharedRef<FChannelMessageNotification, ESPMode::ThreadSafe> ChannelMessage =
		MakeShared<FChannelMessageNotification, ESPMode::ThreadSafe>();
	{
		const FString Prefixed = TEXT("cmc:12345");
		const FTCHARToUTF8 Converted(*Prefixed);
		ChannelMessage->Payload.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	Registry.DispatchMessage(ServerEvent);
	Registry.DispatchMessage(ClientEvent);
	Registry.DispatchMessage(ChannelMessage);

	TestEqual(TEXT("one callback received all three carriers"), Calls, 3);
	TestEqual(TEXT("three deliveries were seen"), SeenOpcodes.Num(), 3);

	if (SeenOpcodes.Num() == 3)
	{
		TestTrue(TEXT("the first carrier is still identifiable as the server event"),
			SeenOpcodes[0] == ECrowdyMessageType::SERVER_EVENT_NOTIFICATION);
		TestTrue(TEXT("the second carrier is still identifiable as the client event"),
			SeenOpcodes[1] == ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION);
		TestTrue(TEXT("the third carrier is still identifiable as the channel message"),
			SeenOpcodes[2] == ECrowdyMessageType::CHANNEL_MESSAGE_NOTIFICATION);
	}

	Subscription.Release();

	Registry.DispatchMessage(ServerEvent);
	Registry.DispatchMessage(ClientEvent);
	Registry.DispatchMessage(ChannelMessage);

	TestEqual(TEXT("releasing the single handle stops all three carriers"), Calls, 3);

	return true;
}

// The regression guard that outlives the redesign: a subscriber that has gone away is never called
// again. A dangling reception layer dispatched to after a world travel is what this exists to stop.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRouterReleasedSubscriberTest,
	"CrowdySDK.Net.Router.ReleasedSubscriberIsNeverCalledAgain", CrowdyRouterTest::RouterTestFlags)
bool FCrowdyRouterReleasedSubscriberTest::RunTest(const FString& Parameters)
{
	using namespace CrowdyRouterTest;

	FCrowdyServiceRegistry Registry;

	int32 Count = 0;

	FCrowdySubscription Subscription = Registry.SubscribeToOpcode(ECrowdyMessageType::GENERIC_SPATIAL_1,
		MakeOptions(ECrowdySubscriptionRole::Observe, TEXT("Subscriber")),
		[&Count](const FCrowdyDelivery&) { ++Count; });

	TestTrue(TEXT("the handle is live after subscribing"), Subscription.IsValid());

	Registry.DispatchMessage(MakeOpcodeMessage(ECrowdyMessageType::GENERIC_SPATIAL_1));
	TestEqual(TEXT("a live subscriber receives the dispatched message"), Count, 1);

	Subscription.Release();
	TestFalse(TEXT("the handle is no longer live after releasing"), Subscription.IsValid());

	Registry.DispatchMessage(MakeOpcodeMessage(ECrowdyMessageType::GENERIC_SPATIAL_1));
	TestEqual(TEXT("a released subscriber receives nothing further"), Count, 1);

	// Releasing twice, and releasing a handle that was never subscribed, are both safe no-ops.
	Subscription.Release();
	FCrowdySubscription Never;
	Never.Release();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
