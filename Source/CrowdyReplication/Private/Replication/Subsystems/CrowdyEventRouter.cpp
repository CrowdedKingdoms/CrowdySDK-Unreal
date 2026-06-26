// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "CrowdyReplicationLog.h"
#include "Components/ActorComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Utils/CrowdyBakedRegistry.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/SerializationFunctionLibrary.h"
#include "Utils/UEventPayloadRegistry.h"

namespace
{
	// An RPC can be dispatched before the spawn event that creates its target entity is
	// drained (the two travel on separate reception layers and tick queues). Retry the
	// call this many ticks before giving up so it fires once the entity registers.
	constexpr int32 CrowdyRpcMaxSpawnWaitAttempts = 600;

	// The RPC was sent from an instance of the function's declaring class — find the
	// matching instance on the target entity: the actor itself, or one of its components.
	UObject* ResolveRpcReceiver(AActor* Actor, const UFunction* Function)
	{
		if (!IsValid(Actor) || !Function)
		{
			return nullptr;
		}

		UClass* OwnerClass = Function->GetOwnerClass();
		if (!OwnerClass)
		{
			return nullptr;
		}

		if (Actor->IsA(OwnerClass))
		{
			return Actor;
		}

		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (IsValid(Component) && Component->IsA(OwnerClass))
			{
				return Component;
			}
		}

		return nullptr;
	}
}

void UCrowdyEventRouter::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

    EntitySubsystem = Collection.InitializeDependency<UCrowdyEntitySubsystem>();

    const UWorld* World = GetWorld();
    UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
    if (!GameInstance)
        return;

    Bridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
    AutoRegistry = GameInstance->GetSubsystem<UCrowdyAutoRegistry>();

    if (!IsValid(Bridge))
    {
        UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEventRouter]: Invalid Bridge subsystem."));
        return;
    }

    if (!LoadConfig())
    {
        UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyEventRouter]: Failed to load config due to incorrect params or disabled by choice."));
        return;
    }

    if (Bridge->ServiceRegistry) Bridge->ServiceRegistry->RegisterReceptionLayer(this);
}

void UCrowdyEventRouter::Deinitialize()
{
    DeferredRpcCalls.Empty();
    AutoRegistry = nullptr;
    Super::Deinitialize();
}

void UCrowdyEventRouter::DispatchEvent(const FCrowdyInboundEvent& Event)
{
	ensure(IsInGameThread());

    const UScriptStruct* StructType = Event.Payload.GetScriptStruct();
    if (!StructType)
    {
        UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyEventRouter] DispatchEvent: payload has no ScriptStruct — dropped"));
        return;
    }

    // RPC-style CrowdyEvents resolve to a UFunction on the target entity rather than to a
    // struct handler, so they take a dedicated path before the handler-map lookup.
    if (StructType == FCrowdyRpcCall::StaticStruct())
    {
        DispatchRpcCall(Event, CrowdyRpcMaxSpawnWaitAttempts);
        return;
    }

    UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Verbose,
        TEXT("[CrowdyEventRouter] DispatchEvent: non-RPC payload '%s' dropped (struct handlers removed)."),
        *StructType->GetName());
}

void UCrowdyEventRouter::DispatchRpcCall(const FCrowdyInboundEvent& Event, int32 RemainingAttempts)
{
    ensure(IsInGameThread());

    const FCrowdyRpcCall& Call = Event.Payload.Get<FCrowdyRpcCall>();
    const FGuid LocalPlayerID = EntitySubsystem ? EntitySubsystem->GetLocalPlayerID() : FGuid();

    // A broadcast echoes back to its sender; drop our own echo so the body — which already ran
    // locally when we sent it — does not run a second time. A targeted (single-actor) message is
    // delivered only to the recipient and never echoes, so there is nothing to drop.
    if (!Event.bTargetedDelivery && LocalPlayerID.IsValid() && Call.SenderID == LocalPlayerID)
        return;

    AActor* TargetEntity = EntitySubsystem ? EntitySubsystem->FindEntity(Call.EntityID) : nullptr;
    if (!TargetEntity)
    {
        if (!Call.EntityID.IsValid())
        {
            // The call carries no entity to resolve and nothing to wait for.
            UE_LOG(LogCrowdyRPC, Verbose,
                TEXT("[CrowdyEventRouter] RPC ignored on receipt — no entity id."));
            return;
        }

        // The entity's spawn event may still be in flight; retry on later ticks.
        DeferRpcCall(Event, RemainingAttempts);
        return;
    }

    UFunction* Function = AutoRegistry ? AutoRegistry->ResolveFunction(Call.ClassID, Call.FunctionID) : nullptr;
    if (!Function)
    {
        // Unknown or drifted signature: drop rather than risk a corrupt dispatch. The call
        // is untrusted network input, so we log instead of asserting (a malformed peer must
        // not be able to spam ensures) — the same stance the serializer takes on bad bytes.
        UE_LOG(LogCrowdyRPC, Warning,
            TEXT("[CrowdyEventRouter] RPC dropped — no function for ClassID=%lld FunctionID=%lld "
                 "(declaring class not loaded, or signature drifted between builds)."),
            Call.ClassID, Call.FunctionID);
        return;
    }

    const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Function);

    // An owner-only or host-only event must run solely on the entity's owner / on the host, which
    // the server guarantees by delivering it as a single-actor message. A broadcast carrying such a
    // function is therefore illegitimate — a misuse or a forgery — and is dropped. That is what
    // keeps "only the owner/host runs it" intact: others can request, never run it themselves. The
    // host then runs it on the target entity regardless of who owns that entity.
    if ((Info.Recipient == ECrowdyEventRecipient::OwningClient
         || Info.Recipient == ECrowdyEventRecipient::Host)
        && !Event.bTargetedDelivery)
    {
        UE_LOG(LogCrowdyRPC, Warning,
            TEXT("[CrowdyEventRouter] RPC dropped — '%s' is owner/host-only but arrived as a broadcast, not a targeted send."),
            *Function->GetName());
        return;
    }

    UObject* Receiver = ResolveRpcReceiver(TargetEntity, Function);
    if (!Receiver)
    {
        UE_LOG(LogCrowdyRPC, Warning,
            TEXT("[CrowdyEventRouter] RPC dropped — entity '%s' carries no '%s' to receive '%s'."),
            *TargetEntity->GetName(), *GetNameSafe(Function->GetOwnerClass()), *Function->GetName());
        return;
    }

    if (FCrowdyRPC::IsRpcTraceEnabled())
    {
        UE_LOG(LogCrowdyRPC, Log,
            TEXT("[CrowdyRPC] recv %s::%s entity=%s sender=%s %s bytes=%d"),
            *GetNameSafe(Receiver->GetClass()), *Function->GetName(),
            *Call.EntityID.ToString(), *Call.SenderID.ToString(),
            Event.bTargetedDelivery ? TEXT("(targeted)") : TEXT("(broadcast)"),
            Call.ParamBlob.Num());
    }

    // The body runs exactly here. A broadcast already ran on the sender and runs once on every
    // other client; an owner-only call runs once, on the owner. No re-announce. The entity scope
    // lets object-reference parameters resolve against this world while the call is decoded.
    FCrowdyRPC::FScopedEntityContext EntityContext(EntitySubsystem);
    FCrowdyRPC::ApplyCall(Receiver, Function, Info, Call);
}

void UCrowdyEventRouter::ReceiveLoopbackCall(const FCrowdyRpcCall& Call)
{
    ensure(IsInGameThread());

    // Replay a call this client just sent through its own receive path so a single client can
    // exercise serialize -> resolve -> dispatch without a second client. Marked as a targeted
    // delivery so it runs exactly once — past the broadcast echo-drop and the owner-only guard —
    // and never re-broadcasts, which is what bounds the loopback.
    FCrowdyInboundEvent Event;
    Event.Payload  = FInstancedStruct::Make(Call);
    Event.bTargetedDelivery = true;
    Event.Target   = ECrowdyTarget::Everyone;
    Event.SenderID = FGuid();
    Event.TargetID = FGuid();

    DispatchRpcCall(Event, CrowdyRpcMaxSpawnWaitAttempts);
}

void UCrowdyEventRouter::ReceiveChannelRpcCall(const FCrowdyRpcCall& Call)
{
    // A channel delivery is a multicast equivalent — not a targeted single-actor send — so the
    // broadcast echo-drop (by SenderID) and the owner/host-only guard both apply, exactly as for a
    // spatial Multicast. The originating client's id rides the payload, so the channel's wire UUID
    // is not needed here.
    FCrowdyInboundEvent Event;
    Event.Payload  = FInstancedStruct::Make(Call);
    Event.bTargetedDelivery = false;
    Event.SenderID = Call.SenderID;
    Event.Target   = ECrowdyTarget::Everyone;
    Event.TargetID = FGuid();

    // Mpsc queue: safe to enqueue off the game thread; Tick drains it through DispatchRpcCall.
    EventQueue.Enqueue(MoveTemp(Event));
}

void UCrowdyEventRouter::DeferRpcCall(const FCrowdyInboundEvent& Event, int32 RemainingAttempts)
{
    if (RemainingAttempts <= 1)
    {
        const FCrowdyRpcCall& Call = Event.Payload.Get<FCrowdyRpcCall>();
        UE_LOG(LogCrowdyRPC, Warning,
            TEXT("[CrowdyEventRouter] RPC for entity %s dropped — entity never spawned within the wait budget."),
            *Call.EntityID.ToString());
        return;
    }

    DeferredRpcCalls.Add({ Event, RemainingAttempts - 1 });
}

void UCrowdyEventRouter::RetryDeferredRpcCalls()
{
    if (DeferredRpcCalls.IsEmpty())
        return;

    // Swap the pending set out first so calls that still can't resolve re-defer into a
    // fresh list for the next tick instead of looping within this one.
    TArray<FCrowdyDeferredRpcCall> Pending = MoveTemp(DeferredRpcCalls);
    DeferredRpcCalls.Reset();

    for (const FCrowdyDeferredRpcCall& Deferred : Pending)
        DispatchRpcCall(Deferred.Event, Deferred.RemainingAttempts);
}

void UCrowdyEventRouter::Tick(float DeltaTime)
{
    // Drain every queued event; the only live path is RPC dispatch (FCrowdyRpcCall).
    FCrowdyInboundEvent Event;
    while (EventQueue.Dequeue(Event))
    {
        DispatchEvent(Event);
    }

    RetryDeferredRpcCalls();
}

bool UCrowdyEventRouter::ShouldCreateSubsystem(UObject* Outer) const
{
    if (!Super::ShouldCreateSubsystem(Outer))
        return false;

    const UWorld* World = Outer ? Outer->GetWorld() : nullptr;

    if (!World)
        return false;

    return (World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
}

void UCrowdyEventRouter::OnMessageReceived(TSharedRef<ICrowdyMessage> Message)
{
    const ECrowdyMessageType MessageType = Message->GetType();
    const bool bTargeted = MessageType == ECrowdyMessageType::SINGLE_ACTOR_MESSAGE;
    if (MessageType != ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION && !bTargeted)
        return;

    // A single-actor message deserializes into an FSingleActorNotification, which derives
    // FGameEventNotification, so the cast is valid for both transports.
    const auto& EventMessage = static_cast<const FGameEventNotification&>(*Message);

    if (EventMessage.State.GetScriptStruct() == nullptr)
        return;

    UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[CrowdyEventRouter] Received event %s"), *EventMessage.GetTypeName().ToString());
    // Events claimed by other reception layers (entity lifecycle, teams, …)
    // never reach this router — FCrowdyServiceRegistry routes them to their
    // claimants and falls back here only for unclaimed types.
    
    FCrowdyInboundEvent InboundEvent;
    InboundEvent.Payload  = EventMessage.State;
    InboundEvent.bTargetedDelivery = bTargeted;
    // A single-actor message's header UUID is the destination, not the sender, so the sender
    // (when a receiver needs it) rides the payload; only a broadcast's UUID identifies the sender.
    InboundEvent.SenderID = bTargeted ? FGuid() : USerializationFunctionLibrary::ToGuid(EventMessage.UUID);
    InboundEvent.Target   = EventMessage.Target;
    InboundEvent.TargetID = EventMessage.TargetID;
    
    UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[CrowdyEventRouter]: Event Payload %s, SenderID: %s, TargetID: %s, Target: %d"), 
        *InboundEvent.Payload.GetScriptStruct()->GetName(),
        *InboundEvent.SenderID.ToString(),
        *InboundEvent.TargetID.ToString(),
        InboundEvent.Target);
    
    EventQueue.Enqueue(MoveTemp(InboundEvent));
}

TArray<ECrowdyMessageType> UCrowdyEventRouter::GetSupportedResponseTypes() const
{
  return {ECrowdyMessageType::CLIENT_EVENT_NOTIFICATION};
}

TArray<UScriptStruct*> UCrowdyEventRouter::GetSupportedEvents() const
{
    // Empty on purpose: the router is the fallback for every event type no
    // other reception layer claims, so it never claims types itself.
    return {};
}

bool UCrowdyEventRouter::LoadConfig() const
{
    const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(GetWorld());
    return Profile && Profile->bEnableNetworking;
}
