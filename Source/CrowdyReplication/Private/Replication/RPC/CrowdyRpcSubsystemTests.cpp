#include "Replication/RPC/CrowdyRpcTestTarget.h"

void UCrowdyRpcSubsystemTestTarget::SubMulticast_Implementation(int32 InValue)
{
	GotValue = InValue;
	++CallCount;
}

void UCrowdyRpcSubsystemTestTarget::SubHostOnly_Implementation(int32 InValue)
{
	GotValue = InValue;
	++CallCount;
}

void UCrowdyRpcSubsystemTestTarget::SubOwnerOnly_Implementation(int32 InValue)
{
	GotValue = InValue;
	++CallCount;
}

void ACrowdyRpcActorTestTarget::ActorOwnerOnly_Implementation(int32 InValue)
{
	GotValue = InValue;
	++CallCount;
}

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/Script.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Subsystem/CrowdyGameSession.h"

// Subsystem RPC over the channel (Subsystem Replication Phase 2). These tests exercise a non-actor (subsystem)
// participant on the RPC plane: the pure DecideRoute policy table (send-side identity + recipient routing), a
// Multicast channel round-trip onto an enrolled participant, the host-only channel gate (run only where local is
// host), and the actor-path regression (owner/host-only broadcasts to an actor are still dropped). A plain UObject
// fires ProcessEvent without a world; only the actor regression needs an editor world.
namespace
{
	constexpr EAutomationTestFlags CrowdyRpcSubsystemTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// UCrowdyAutoRegistry is a UGameInstanceSubsystem (ClassWithin=UGameInstance); a transient-package NewObject
	// trips a ClassWithin ensure, so outer it to a bare GameInstance (the established test idiom). Register the
	// fixture classes' RPC functions so ResolveFunction can find them on receipt.
	UCrowdyAutoRegistry* MakeRpcRegistry()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		UCrowdyAutoRegistry* Registry = NewObject<UCrowdyAutoRegistry>(GameInstance);
		Registry->UpdateClassRpcFunctions(UCrowdyRpcSubsystemTestTarget::StaticClass());
		Registry->UpdateClassRpcFunctions(ACrowdyRpcActorTestTarget::StaticClass());
		return Registry;
	}

	// A bare entity subsystem (UWorldSubsystem, no ClassWithin) with a local player id set, so
	// RegisterParticipant / FindParticipant / GetLocalPlayerID resolve headlessly.
	UCrowdyEntitySubsystem* MakeEntitySubsystem(const FGuid& LocalPlayer)
	{
		UCrowdyEntitySubsystem* ES = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
		ES->SetLocalPlayerID(LocalPlayer);
		return ES;
	}

	UCrowdyEventRouter* MakeRouter(UCrowdyAutoRegistry* Registry, UCrowdyEntitySubsystem* Entities)
	{
		UCrowdyEventRouter* Router = NewObject<UCrowdyEventRouter>(GetTransientPackage());
		Router->SetAutoRegistryForTest(Registry);
		Router->SetEntitySubsystemForTest(Entities);
		return Router;
	}

	// A real game session carrying an elected host, so GetHostID() read-through resolves. UCrowdyGameSession is a
	// UGameInstanceSubsystem (ClassWithin=UGameInstance), so it needs a GameInstance outer.
	UCrowdyGameSession* MakeGameSession(const FGuid& HostID)
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		UCrowdyGameSession* Session = NewObject<UCrowdyGameSession>(GameInstance);
		Session->SetHostID(HostID);
		return Session;
	}

	// Registers any UObject as a resolvable RemoteProxy participant so FindParticipant(EntityId) returns it (its
	// OwnerID is a fresh non-local guid, so IsLocallyOwned is false and the owned-entity gates are bypassed).
	void RegisterProxyParticipant(UCrowdyEntitySubsystem* Entities, const FGuid& EntityId, UObject* Participant)
	{
		FCrowdyEntityRecord Rec;
		Rec.NetID = EntityId;
		Rec.OwnerID = FGuid::NewGuid();
		Rec.Role = ECrowdyRole::RemoteProxy;
		Rec.Participant = Participant;
		Entities->RegisterEntity(Rec);
	}

	FCrowdyInboundEvent MakeInboundRpcEvent(const FCrowdyRpcCall& Call, bool bTargeted)
	{
		FCrowdyInboundEvent Event;
		Event.Payload = FInstancedStruct::Make(Call);
		Event.bTargetedDelivery = bTargeted;
		Event.SenderID = Call.SenderID;
		Event.Target = ECrowdyTarget::Everyone;
		return Event;
	}

	// Marshals a single-int32 CrowdyEvent into a wire-ready call, then stamps the routing identity (EntityID /
	// SenderID) the receive path reads. ClassID + FunctionID come from MarshalCall over the receiver's signature.
	template <typename T>
	FCrowdyRpcCall MakeRpcCall(const TCHAR* ImplName, void (T::*MemberFn)(int32), int32 Value,
		const FGuid& EntityID, const FGuid& SenderID)
	{
		FCrowdyRpcCall Call;
		if (UFunction* Fn = FCrowdyRPC::ResolveFunction(T::StaticClass(), ImplName))
		{
			const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Fn);
			Call = FCrowdyRPC::MarshalCall(Fn, Info, MemberFn, Value);
		}
		Call.EntityID = EntityID;
		Call.SenderID = SenderID;
		return Call;
	}

	// A worldless-friendly editor world: only the actor-path regression needs one, since an AActor's ProcessEvent
	// no-ops without a world (a plain UObject does not). The held FEditorScriptExecutionGuard flips
	// GAllowActorScriptExecutionInEditor so ProcessEvent runs; an editor world never creates the PIE/Game-gated
	// Crowdy subsystems, so it tears down cleanly (mirrors the Phase 4 apply tests).
	struct FCrowdyRpcTestWorld
	{
		FEditorScriptExecutionGuard ScriptGuard;
		UWorld* World = nullptr;

		FCrowdyRpcTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyRpcTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		template <typename T>
		T* Spawn()
		{
			return World->SpawnActor<T>();
		}
	};
}

// The pure ownership-model policy for every (recipient x participant-kind x authority) combination. The actor
// rows reproduce SerializeAndRoute's shipped behavior; a non-spatial SpatialMulticast is hard-rejected, and every
// other non-spatial recipient routes over the channel.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcSubsystemDecideRouteTableTest,
	"CrowdySDK.RPC.SubsystemDecideRouteTable", CrowdyRpcSubsystemTestFlags)
bool FCrowdyRpcSubsystemDecideRouteTableTest::RunTest(const FString& Parameters)
{
	auto Check = [this](const TCHAR* Label, ECrowdyEventRecipient Recipient, bool bNonSpatial, bool bValid,
		bool bOwn, bool bHost, bool bExpRun, ECrowdyRpcRoute ExpRoute)
	{
		const FCrowdyRpcRouteDecision D = FCrowdyRPC::DecideRoute(Recipient, bNonSpatial, bValid, bOwn, bHost);
		TestEqual(*FString::Printf(TEXT("%s run"), Label), D.bRunLocally, bExpRun);
		TestEqual(*FString::Printf(TEXT("%s route"), Label), static_cast<int32>(D.Route), static_cast<int32>(ExpRoute));
	};

	// Actor rows (bNonSpatial == false) reproduce today's switch exactly.
	Check(TEXT("actor OwningClient owner"), ECrowdyEventRecipient::OwningClient, false, /*valid*/true, /*own*/true, /*host*/false,
		true, ECrowdyRpcRoute::None);
	Check(TEXT("actor OwningClient untracked"), ECrowdyEventRecipient::OwningClient, false, /*valid*/false, /*own*/false, /*host*/false,
		true, ECrowdyRpcRoute::None);
	Check(TEXT("actor OwningClient non-owner"), ECrowdyEventRecipient::OwningClient, false, /*valid*/true, /*own*/false, /*host*/false,
		false, ECrowdyRpcRoute::SingleActorToOwner);
	Check(TEXT("actor Host host"), ECrowdyEventRecipient::Host, false, /*valid*/true, /*own*/false, /*host*/true,
		true, ECrowdyRpcRoute::None);
	Check(TEXT("actor Host non-host"), ECrowdyEventRecipient::Host, false, /*valid*/true, /*own*/false, /*host*/false,
		false, ECrowdyRpcRoute::SingleActorToHost);
	Check(TEXT("actor Multicast"), ECrowdyEventRecipient::Multicast, false, /*valid*/true, /*own*/false, /*host*/false,
		true, ECrowdyRpcRoute::Channel);
	Check(TEXT("actor SpatialMulticast"), ECrowdyEventRecipient::SpatialMulticast, false, /*valid*/true, /*own*/false, /*host*/false,
		true, ECrowdyRpcRoute::SpatialBroadcast);

	// Non-spatial rows (bNonSpatial == true).
	Check(TEXT("nonspatial SpatialMulticast"), ECrowdyEventRecipient::SpatialMulticast, true, /*valid*/true, /*own*/false, /*host*/false,
		false, ECrowdyRpcRoute::Reject);
	Check(TEXT("nonspatial Multicast"), ECrowdyEventRecipient::Multicast, true, /*valid*/true, /*own*/false, /*host*/false,
		true, ECrowdyRpcRoute::Channel);
	Check(TEXT("nonspatial OwningClient owner"), ECrowdyEventRecipient::OwningClient, true, /*valid*/true, /*own*/true, /*host*/false,
		true, ECrowdyRpcRoute::Channel);
	Check(TEXT("nonspatial OwningClient untracked"), ECrowdyEventRecipient::OwningClient, true, /*valid*/false, /*own*/false, /*host*/false,
		true, ECrowdyRpcRoute::Channel);
	Check(TEXT("nonspatial OwningClient non-owner"), ECrowdyEventRecipient::OwningClient, true, /*valid*/true, /*own*/false, /*host*/false,
		false, ECrowdyRpcRoute::Channel);
	Check(TEXT("nonspatial Host host"), ECrowdyEventRecipient::Host, true, /*valid*/true, /*own*/false, /*host*/true,
		true, ECrowdyRpcRoute::Channel);
	Check(TEXT("nonspatial Host non-host"), ECrowdyEventRecipient::Host, true, /*valid*/true, /*own*/false, /*host*/false,
		false, ECrowdyRpcRoute::Channel);

	return true;
}

// A Multicast RPC targeting an enrolled subsystem participant (foreign owner, foreign sender) applies on receipt:
// no owner/host gate, the body runs exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcSubsystemMulticastRoundTripTest,
	"CrowdySDK.RPC.SubsystemMulticastRoundTrip", CrowdyRpcSubsystemTestFlags)
bool FCrowdyRpcSubsystemMulticastRoundTripTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeRpcRegistry();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	UCrowdyRpcSubsystemTestTarget* Target = NewObject<UCrowdyRpcSubsystemTestTarget>();
	const FGuid EntityID = FGuid::NewGuid();
	RegisterProxyParticipant(Entities, EntityID, Target);

	const FCrowdyRpcCall Call = MakeRpcCall(TEXT("SubMulticast_Implementation"),
		&UCrowdyRpcSubsystemTestTarget::SubMulticast_Implementation, 42, EntityID, FGuid::NewGuid());

	Router->DispatchEvent(MakeInboundRpcEvent(Call, /*bTargeted=*/false));

	TestEqual(TEXT("Multicast RPC applied the value"), Target->GotValue, 42);
	TestEqual(TEXT("Multicast RPC ran exactly once"), Target->CallCount, 1);
	return true;
}

// A host-only subsystem RPC arrives over the channel as a broadcast (no single-actor transport for a non-actor):
// it runs ONLY where local is the host, and drops quietly elsewhere.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcSubsystemHostOnlyGatedByHostTest,
	"CrowdySDK.RPC.SubsystemHostOnlyGatedByHost", CrowdyRpcSubsystemTestFlags)
bool FCrowdyRpcSubsystemHostOnlyGatedByHostTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeRpcRegistry();

	// (a) local is NOT the host: the host-only event drops.
	{
		const FGuid LocalPlayer = FGuid::NewGuid();
		UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(LocalPlayer);
		Entities->SetGameSessionForTest(MakeGameSession(FGuid::NewGuid())); // someone else is host
		UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

		UCrowdyRpcSubsystemTestTarget* Target = NewObject<UCrowdyRpcSubsystemTestTarget>();
		const FGuid EntityID = Entities->RegisterParticipant(Target, ECrowdyOwnership::Host);

		const FCrowdyRpcCall Call = MakeRpcCall(TEXT("SubHostOnly_Implementation"),
			&UCrowdyRpcSubsystemTestTarget::SubHostOnly_Implementation, 11, EntityID, FGuid::NewGuid());
		Router->DispatchEvent(MakeInboundRpcEvent(Call, /*bTargeted=*/false));

		TestEqual(TEXT("non-host does NOT run the host-only subsystem RPC"), Target->CallCount, 0);
	}

	// (b) local IS the host: the host-only event runs once.
	{
		const FGuid LocalPlayer = FGuid::NewGuid();
		UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(LocalPlayer);
		Entities->SetGameSessionForTest(MakeGameSession(LocalPlayer)); // local is host
		UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

		UCrowdyRpcSubsystemTestTarget* Target = NewObject<UCrowdyRpcSubsystemTestTarget>();
		const FGuid EntityID = Entities->RegisterParticipant(Target, ECrowdyOwnership::Host);

		const FCrowdyRpcCall Call = MakeRpcCall(TEXT("SubHostOnly_Implementation"),
			&UCrowdyRpcSubsystemTestTarget::SubHostOnly_Implementation, 11, EntityID, FGuid::NewGuid());
		Router->DispatchEvent(MakeInboundRpcEvent(Call, /*bTargeted=*/false));

		TestEqual(TEXT("host runs the host-only subsystem RPC once"), Target->CallCount, 1);
		TestEqual(TEXT("host applied the value"), Target->GotValue, 11);
	}

	return true;
}

// Regression: an owner/host-only RPC arriving as a BROADCAST for an ACTOR participant is still dropped (actor path
// unchanged); the same call as a TARGETED single-actor send runs. Needs an editor world so the actor's ProcessEvent
// fires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRpcSubsystemActorOwnerHostBroadcastStillDroppedTest,
	"CrowdySDK.RPC.SubsystemActorOwnerHostBroadcastStillDropped", CrowdyRpcSubsystemTestFlags)
bool FCrowdyRpcSubsystemActorOwnerHostBroadcastStillDroppedTest::RunTest(const FString& Parameters)
{
	UCrowdyAutoRegistry* Registry = MakeRpcRegistry();
	UCrowdyEntitySubsystem* Entities = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEventRouter* Router = MakeRouter(Registry, Entities);

	FCrowdyRpcTestWorld TestWorld;
	ACrowdyRpcActorTestTarget* Actor = TestWorld.Spawn<ACrowdyRpcActorTestTarget>();
	if (!TestNotNull(TEXT("actor spawned into the editor world"), Actor))
	{
		return false;
	}

	const FGuid EntityID = FGuid::NewGuid();
	RegisterProxyParticipant(Entities, EntityID, Actor);

	// Broadcast owner-only to an actor: dropped by the actor branch (must arrive as a targeted send).
	{
		const FCrowdyRpcCall Call = MakeRpcCall(TEXT("ActorOwnerOnly_Implementation"),
			&ACrowdyRpcActorTestTarget::ActorOwnerOnly_Implementation, 7, EntityID, FGuid::NewGuid());
		Router->DispatchEvent(MakeInboundRpcEvent(Call, /*bTargeted=*/false));
		TestEqual(TEXT("broadcast owner-only to an actor is dropped"), Actor->CallCount, 0);
	}

	// The same call as a targeted single-actor send passes the gate and runs.
	{
		const FCrowdyRpcCall Call = MakeRpcCall(TEXT("ActorOwnerOnly_Implementation"),
			&ACrowdyRpcActorTestTarget::ActorOwnerOnly_Implementation, 7, EntityID, FGuid::NewGuid());
		Router->DispatchEvent(MakeInboundRpcEvent(Call, /*bTargeted=*/true));
		TestEqual(TEXT("targeted owner-only to an actor runs once"), Actor->CallCount, 1);
		TestEqual(TEXT("targeted owner-only applied the value"), Actor->GotValue, 7);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
