#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/UDP/Subscription/FCrowdyPayloadKey.h"
#include "Data/CrowdyEntityTypes.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/CrowdyRpcTestTarget.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Replication/Subsystems/CrowdyStateTestSupport.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Utils/UCrowdyClassRegistry.h"
#include "Utils/UEventPayloadRegistry.h"

// Inbound messages are read on the game thread and applied there, so a message that has been delivered has
// already had its effect: nothing is parked on a queue for a later frame. These cover the two paths that used
// to park, plus the one per-frame job that legitimately remains (the spawn-wait retry ladder).
namespace
{
	constexpr EAutomationTestFlags CrowdyInboundTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Carries a decoded event payload under its registered type number, which is how the router matches an
	// event to its subscribers. Nothing else about the message matters to these cases.
	struct FCrowdyInboundTestMessage : ICrowdyMessage
	{
		FInstancedStruct EventPayload;
		FCrowdyTypeID TypeID = 0;

		virtual ECrowdyMessageType GetType() const override { return ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION; }
		virtual FName GetTypeName() const override { return TEXT("Inbound Delivery Test Message"); }
		virtual TArray<uint8> Serialize() const override { return {}; }
		[[nodiscard]] virtual bool DecodePayload(const FCrowdyFrame&) override { return true; }
		virtual FCrowdyPayloadKey GetPayloadKey() const override { return FCrowdyPayloadKey::Event(TypeID); }
		virtual const FInstancedStruct* GetPayload() const override
		{
			return EventPayload.IsValid() ? &EventPayload : nullptr;
		}
	};

	template <typename TEvent>
	TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> MakeInboundTestMessage(const TEvent& Event)
	{
		TSharedRef<FCrowdyInboundTestMessage, ESPMode::ThreadSafe> Message =
			MakeShared<FCrowdyInboundTestMessage, ESPMode::ThreadSafe>();
		Message->EventPayload = FInstancedStruct::Make(Event);
		UEventPayloadRegistry::Get()->GetID(TEvent::StaticStruct(), Message->TypeID);
		return Message;
	}

	// The wire carrier for a broadcast game event, with its payload already decoded the way the parser
	// leaves it. The router's fallback reads this shape directly, so a test built on it exercises the
	// production wire ingress rather than a stand-in for it.
	struct FCrowdyInboundTestEventNotification : FGameEventNotification
	{
		virtual FName GetTypeName() const override { return TEXT("Inbound Delivery Test Notification"); }
	};

	// The event type number is what the registry routes on, so the payload struct is registered here to
	// give it the same number the send path would resolve.
	template <typename TEvent>
	TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe> MakeInboundTestNotification(const TEvent& Event)
	{
		TSharedRef<FCrowdyInboundTestEventNotification, ESPMode::ThreadSafe> Message =
			MakeShared<FCrowdyInboundTestEventNotification, ESPMode::ThreadSafe>();
		Message->State = FInstancedStruct::Make(Event);
		UEventPayloadRegistry::Get()->RegisterStructAuto(TEvent::StaticStruct());
		UEventPayloadRegistry::Get()->GetID(TEvent::StaticStruct(), Message->EventType);
		return Message;
	}

	// An entity already known to the registry, so a later spawn event for the same id takes the
	// update-in-place branch and needs no world to spawn into.
	void RegisterKnownEntity(UCrowdyEntitySubsystem* Entities, const FGuid& NetID, const FGuid& OwnerID)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = NetID;
		Record.OwnerID = OwnerID;
		Record.Role = ECrowdyRole::RemoteProxy;
		Entities->RegisterEntity(Record);
	}
}

// A remote spawn event has taken effect by the time the dispatch that carried it returns. Nothing is ticked
// between the two, so a queue between delivery and application would leave the record untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInboundRemoteSpawnAppliesDuringDispatchTest,
	"CrowdySDK.Inbound.RemoteSpawnAppliesDuringDispatch", CrowdyInboundTestFlags)
bool FCrowdyInboundRemoteSpawnAppliesDuringDispatchTest::RunTest(const FString& Parameters)
{
	FCrowdyServiceRegistry Registry;

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	Entities->SubscribeToSpawnDestroyForTest(Registry);

	// Registered with nothing known about it yet, because backfilling those two fields is the only thing a
	// spawn event may do to a record that already exists. A settled owner is deliberately not re-pointable
	// from the wire, so observing synchronicity through an owner REWRITE would be observing a refusal.
	const FGuid EntityID = FGuid::NewGuid();
	RegisterKnownEntity(Entities, EntityID, FGuid());

	FCrowdyEntitySpawnEvent Spawn;
	Spawn.EntityID = EntityID;
	Spawn.OwnerID = FGuid::NewGuid();
	Spawn.ClassID = 4242;

	Registry.DispatchMessage(MakeInboundTestMessage(Spawn));

	const FCrowdyEntityRecord* Record = Entities->FindRecord(EntityID);
	if (!TestNotNull(TEXT("the entity is still registered"), Record))
	{
		return false;
	}

	TestEqual(TEXT("the spawn event's owner was applied before the dispatch returned"), Record->OwnerID, Spawn.OwnerID);
	TestEqual(TEXT("the spawn event's class id was applied before the dispatch returned"),
		static_cast<int64>(Record->ClassID), static_cast<int64>(Spawn.ClassID));
	return true;
}

// A remote destroy event has taken effect by the time the dispatch that carried it returns: the entity is
// gone from the registry with no tick in between.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInboundRemoteDestroyAppliesDuringDispatchTest,
	"CrowdySDK.Inbound.RemoteDestroyAppliesDuringDispatch", CrowdyInboundTestFlags)
bool FCrowdyInboundRemoteDestroyAppliesDuringDispatchTest::RunTest(const FString& Parameters)
{
	FCrowdyServiceRegistry Registry;

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	Entities->SubscribeToSpawnDestroyForTest(Registry);

	const FGuid EntityID = FGuid::NewGuid();
	RegisterKnownEntity(Entities, EntityID, FGuid::NewGuid());

	FCrowdyEntityDestroyEvent Destroy;
	Destroy.EntityID = EntityID;

	Registry.DispatchMessage(MakeInboundTestMessage(Destroy));

	TestNull(TEXT("the entity was unregistered before the dispatch returned"), Entities->FindRecord(EntityID));
	return true;
}

// A reliable RPC arriving over the session channel runs its body before ReceiveChannelRpcCall returns.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInboundChannelRpcAppliesBeforeReturnTest,
	"CrowdySDK.Inbound.ChannelRpcAppliesBeforeReturn", CrowdyInboundTestFlags)
bool FCrowdyInboundChannelRpcAppliesBeforeReturnTest::RunTest(const FString& Parameters)
{
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UCrowdyAutoRegistry* Registry = NewObject<UCrowdyAutoRegistry>(GameInstance);
	Registry->UpdateClassRpcFunctions(UCrowdyRpcSubsystemTestTarget::StaticClass());

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyRpcSubsystemTestTarget* Target = NewObject<UCrowdyRpcSubsystemTestTarget>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterProxyParticipant(Entities, EntityID, Target);

	FCrowdyRpcCall Call;
	if (UFunction* Fn = FCrowdyRPC::ResolveFunction(UCrowdyRpcSubsystemTestTarget::StaticClass(),
		TEXT("SubMulticast_Implementation")))
	{
		const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
		Call = FCrowdyRPC::MarshalCall(Fn, Info, &UCrowdyRpcSubsystemTestTarget::SubMulticast_Implementation, 42);
	}
	Call.EntityID = EntityID;
	Call.SenderID = FGuid::NewGuid();

	Router->ReceiveChannelRpcCall(Call);

	TestEqual(TEXT("the channel RPC ran before ReceiveChannelRpcCall returned"), Target->GotValue, 42);
	TestEqual(TEXT("the channel RPC ran exactly once"), Target->CallCount, 1);
	return true;
}

// The router's fallback is the ingress for every event off the wire that nothing else claims, which is where
// spatial and broadcast RPCs arrive. A call routed through it runs its body before DispatchMessage returns,
// and nothing is left for a later tick.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInboundWireEventRpcAppliesDuringDispatchTest,
	"CrowdySDK.Inbound.WireEventRpcAppliesDuringDispatch", CrowdyInboundTestFlags)
bool FCrowdyInboundWireEventRpcAppliesDuringDispatchTest::RunTest(const FString& Parameters)
{
	FCrowdyServiceRegistry ServiceRegistry;

	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UCrowdyAutoRegistry* AutoRegistry = NewObject<UCrowdyAutoRegistry>(GameInstance);
	AutoRegistry->UpdateClassRpcFunctions(UCrowdyRpcSubsystemTestTarget::StaticClass());

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(AutoRegistry, Entities);
	Router->SubscribeToEventFallbackForTest(ServiceRegistry);

	UCrowdyRpcSubsystemTestTarget* Target = NewObject<UCrowdyRpcSubsystemTestTarget>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterProxyParticipant(Entities, EntityID, Target);

	FCrowdyRpcCall Call;
	if (UFunction* Fn = FCrowdyRPC::ResolveFunction(UCrowdyRpcSubsystemTestTarget::StaticClass(),
		TEXT("SubMulticast_Implementation")))
	{
		const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
		Call = FCrowdyRPC::MarshalCall(Fn, Info, &UCrowdyRpcSubsystemTestTarget::SubMulticast_Implementation, 7);
	}
	Call.EntityID = EntityID;
	Call.SenderID = FGuid::NewGuid(); // foreign, so the broadcast echo-drop never fires

	ServiceRegistry.DispatchMessage(MakeInboundTestNotification(Call));

	TestEqual(TEXT("the RPC ran before DispatchMessage returned"), Target->GotValue, 7);
	TestEqual(TEXT("the RPC ran exactly once"), Target->CallCount, 1);
	TestEqual(TEXT("nothing was held for a later tick"), Router->NumDeferredForTest(), 0);
	return true;
}

// The wait queue is the only place the receive path still copies a payload, and this is what says that copy
// is real. The call enters through the production wire fallback, the message that carried it is destroyed,
// and only then is the target registered and the retry run. What is asserted is the VALUE that arrives,
// because an event whose payload did not survive is consumed without dispatching, which empties the queue
// exactly as a delivery does: a count alone cannot tell the two apart.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInboundDeferredWireEventKeepsItsPayloadTest,
	"CrowdySDK.Inbound.DeferredWireEventKeepsItsPayload", CrowdyInboundTestFlags)
bool FCrowdyInboundDeferredWireEventKeepsItsPayloadTest::RunTest(const FString& Parameters)
{
	FCrowdyServiceRegistry ServiceRegistry;

	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UCrowdyAutoRegistry* AutoRegistry = NewObject<UCrowdyAutoRegistry>(GameInstance);
	AutoRegistry->UpdateClassRpcFunctions(UCrowdyRpcSubsystemTestTarget::StaticClass());

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(AutoRegistry, Entities);
	Router->SubscribeToEventFallbackForTest(ServiceRegistry);

	FCrowdyRpcCall Call;
	if (UFunction* Fn = FCrowdyRPC::ResolveFunction(UCrowdyRpcSubsystemTestTarget::StaticClass(),
		TEXT("SubMulticast_Implementation")))
	{
		const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
		Call = FCrowdyRPC::MarshalCall(Fn, Info, &UCrowdyRpcSubsystemTestTarget::SubMulticast_Implementation, 4242);
	}

	const FGuid EntityID = FGuid::NewGuid();
	Call.EntityID = EntityID;
	Call.SenderID = FGuid::NewGuid(); // foreign, so the broadcast echo-drop never fires

	// Nothing holds this id yet, so the call is queued. The message is a temporary on purpose: it is gone
	// by the next statement, and with it the payload the router was handed a view of.
	ServiceRegistry.DispatchMessage(MakeInboundTestNotification(Call));

	if (!TestEqual(TEXT("the call is held while its entity is unregistered"), Router->NumDeferredForTest(), 1))
	{
		return false;
	}

	UCrowdyRpcSubsystemTestTarget* Target = NewObject<UCrowdyRpcSubsystemTestTarget>();
	RegisterProxyParticipant(Entities, EntityID, Target);

	Router->RetryDeferredForTest();

	TestEqual(TEXT("the retried call carried the parameter its sender wrote"), Target->GotValue, 4242);
	TestEqual(TEXT("and it ran exactly once"), Target->CallCount, 1);
	TestEqual(TEXT("nothing is left held"), Router->NumDeferredForTest(), 0);
	return true;
}

// A CrowdyState delta arriving over the session channel is decoded onto its target, and fires that target's
// OnRep, before ReceiveChannelStateDelta returns.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInboundChannelStateDeltaAppliesBeforeReturnTest,
	"CrowdySDK.Inbound.ChannelStateDeltaAppliesBeforeReturn", CrowdyInboundTestFlags)
bool FCrowdyInboundChannelStateDeltaAppliesBeforeReturnTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* TargetClass = UCrowdyStateSubsystemTestTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("the participant class has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateSubsystemTestTarget* Source = NewObject<UCrowdyStateSubsystemTestTarget>();
	Source->RepInt = 42;
	Source->RepNotified = 7;

	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	for (const TCHAR* Name : { TEXT("RepInt"), TEXT("RepNotified") })
	{
		const int32 Index = IndexOfPropertyName(*Layout, Name);
		if (TestTrue(*FString::Printf(TEXT("%s present in layout"), Name), Index != INDEX_NONE))
		{
			Dirty[Index] = true;
		}
	}

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	Delta.EntityID = FGuid::NewGuid();
	Delta.SenderID = FGuid::NewGuid(); // foreign, so the self-echo drop never fires
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	UCrowdyStateSubsystemTestTarget* Target = NewObject<UCrowdyStateSubsystemTestTarget>();
	RegisterProxyParticipant(Entities, Delta.EntityID, Target);

	Router->ReceiveChannelStateDelta(Delta);

	TestEqual(TEXT("RepInt applied before ReceiveChannelStateDelta returned"), Target->RepInt, 42);
	TestEqual(TEXT("RepNotified applied before ReceiveChannelStateDelta returned"), Target->RepNotified, 7);
	TestEqual(TEXT("OnRep fired before ReceiveChannelStateDelta returned"), Target->NotifiedOnRepCount, 1);
	return true;
}

// An event addressed to an entity that has not registered yet is held and retried, and Tick is what drives
// the retry: the delta applies on the tick that follows the target's registration, not on delivery.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyInboundTickRetriesDeferredEventTest,
	"CrowdySDK.Inbound.TickRetriesDeferredEvent", CrowdyInboundTestFlags)
bool FCrowdyInboundTickRetriesDeferredEventTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	if (!TestNotNull(TEXT("registry created"), Registry))
	{
		return false;
	}

	UClass* TargetClass = UCrowdyStateSubsystemTestTarget::StaticClass();
	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(TargetClass);
	if (!TestNotNull(TEXT("the participant class has a layout"), Layout))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyStateSubsystemTestTarget* Source = NewObject<UCrowdyStateSubsystemTestTarget>();
	Source->RepInt = 123;

	TBitArray<> Dirty;
	Dirty.Init(false, Layout->Properties.Num());
	const int32 RepIntIndex = IndexOfPropertyName(*Layout, TEXT("RepInt"));
	if (TestTrue(TEXT("RepInt present in layout"), RepIntIndex != INDEX_NONE))
	{
		Dirty[RepIntIndex] = true;
	}

	TArray<uint8> Blob;
	FCrowdyStateCodec::Encode(*Layout, Source, Dirty, /*bKeyframe=*/false, Blob);

	FCrowdyStateDelta Delta;
	Delta.ClassID = static_cast<int64>(UCrowdyClassRegistry::Get()->GetID(TargetClass));
	Delta.EntityID = FGuid::NewGuid();
	Delta.SenderID = FGuid::NewGuid();
	Delta.LayoutHash = Layout->LayoutHash;
	Delta.Blob = Blob;

	// The target does not exist yet, so the delta is held rather than applied.
	Router->ReceiveChannelStateDelta(Delta);
	TestEqual(TEXT("the delta is held while its target is unregistered"), Router->NumDeferredForTest(), 1);

	// A tick with the target still missing keeps it held rather than dropping it.
	Router->Tick(0.016f);
	TestEqual(TEXT("a tick with the target still missing keeps the delta held"), Router->NumDeferredForTest(), 1);

	UCrowdyStateSubsystemTestTarget* Target = NewObject<UCrowdyStateSubsystemTestTarget>();
	RegisterProxyParticipant(Entities, Delta.EntityID, Target);

	// Registration alone does not apply it; the retry ladder runs from Tick.
	TestEqual(TEXT("registering the target does not itself apply the held delta"), Target->RepInt, 0);

	Router->Tick(0.016f);

	TestEqual(TEXT("Tick retried the held delta onto the now-registered target"), Target->RepInt, 123);
	TestEqual(TEXT("nothing is left held"), Router->NumDeferredForTest(), 0);
	return true;
}

#endif
