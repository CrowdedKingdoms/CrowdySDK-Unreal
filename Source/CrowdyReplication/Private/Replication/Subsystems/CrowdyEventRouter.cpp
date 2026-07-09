// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "CrowdyReplicationLog.h"
#include "Components/ActorComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/Subsystems/CrowdyStateReplicator.h"
#include "Utils/UCrowdyClassRegistry.h"
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

	// A state delta races its target entity's spawn event the same way an RPC does; retry it on later
	// ticks with the same budget so it applies once the entity registers.
	constexpr int32 CrowdyStateMaxSpawnWaitAttempts = 600;

	// The RPC was sent from an instance of the function's declaring class find the matching instance on the
	// target participant: the participant itself, or (when it is an actor) one of its components.
	UObject* ResolveRpcReceiver(UObject* Participant, const UFunction* Function)
	{
		if (!IsValid(Participant) || !Function)
		{
			return nullptr;
		}

		UClass* OwnerClass = Function->GetOwnerClass();
		if (!OwnerClass)
		{
			return nullptr;
		}

		if (Participant->IsA(OwnerClass))
		{
			return Participant;
		}

		// Only an actor carries components to fall back to; any other UObject resolves solely by IsA above.
		if (AActor* Actor = Cast<AActor>(Participant))
		{
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (IsValid(Component) && Component->IsA(OwnerClass))
				{
					return Component;
				}
			}
		}

		return nullptr;
	}

	UObject* ResolveRpcReceiver(AActor* Actor, const UFunction* Function)
	{
		return ResolveRpcReceiver(static_cast<UObject*>(Actor), Function);
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
    DeferredEvents.Empty();
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

    // CrowdyState deltas resolve to a live container on the target entity and apply their positional body
    // onto it, so they take a dedicated path alongside the RPC one.
    if (StructType == FCrowdyStateDelta::StaticStruct())
    {
        DispatchStateDelta(Event, CrowdyStateMaxSpawnWaitAttempts);
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

    // A broadcast echoes back to its sender; drop our own echo so the body which already ran
    // locally when we sent it does not run a second time. A targeted (single-actor) message is
    // delivered only to the recipient and never echoes, so there is nothing to drop.
    if (!Event.bTargetedDelivery && LocalPlayerID.IsValid() && Call.SenderID == LocalPlayerID)
        return;

    // Resolve the target as a PARTICIPANT, not strictly an actor: a non-actor participant (a subsystem enrolled
    // via RegisterParticipant) receives RPCs over the channel the same as an actor receives them over the wire.
    UObject* TargetParticipant = EntitySubsystem ? EntitySubsystem->FindParticipant(Call.EntityID) : nullptr;
    if (!TargetParticipant)
    {
        if (!Call.EntityID.IsValid())
        {
            // The call carries no entity to resolve and nothing to wait for.
            UE_LOG(LogCrowdyRPC, Verbose,
                TEXT("[CrowdyEventRouter] RPC ignored on receipt — no entity id."));
            return;
        }

        // The entity's spawn event may still be in flight; retry on later ticks.
        DeferEvent(Event, RemainingAttempts);
        return;
    }

    UFunction* Function = AutoRegistry ? AutoRegistry->ResolveFunction(Call.ClassID, Call.FunctionID) : nullptr;
    if (!Function)
    {
        // Unknown or drifted signature: drop rather than risk a corrupt dispatch. The call
        // is untrusted network input, so we log instead of asserting (a malformed peer must
        // not be able to spam ensures) the same stance the serializer takes on bad bytes.
        UE_LOG(LogCrowdyRPC, Warning,
            TEXT("[CrowdyEventRouter] RPC dropped — no function for ClassID=%lld FunctionID=%lld "
                 "(declaring class not loaded, or signature drifted between builds)."),
            Call.ClassID, Call.FunctionID);
        return;
    }

    const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Function);

    // An owner-only or host-only event must run solely on the entity's owner / on the host. How that is
    // guaranteed differs by participant kind:
    //   - Actor: the server delivers it as a single-actor (targeted) message. A broadcast carrying such a
    //     function is illegitimate (misuse or forgery) and is dropped. (unchanged behavior)
    //   - Non-spatial participant (a subsystem): there is no single-actor transport for a non-actor, so an
    //     owner/host-only event legitimately arrives as a channel broadcast. Gate it by identity so only the
    //     intended client runs it. Precedence-by-convention, NOT enforced (a forged sender is honored)
    //     cheat-sensitive state belongs in Game Models, not on this view plane.
    const bool bIsActorParticipant = Cast<AActor>(TargetParticipant) != nullptr;
    if (Info.Recipient == ECrowdyEventRecipient::OwningClient
        || Info.Recipient == ECrowdyEventRecipient::Host)
    {
        if (bIsActorParticipant)
        {
            if (!Event.bTargetedDelivery)
            {
                UE_LOG(LogCrowdyRPC, Warning,
                    TEXT("[CrowdyEventRouter] RPC dropped — '%s' is owner/host-only but arrived as a broadcast, not a targeted send."),
                    *Function->GetName());
                return;
            }
        }
        else if (Info.Recipient == ECrowdyEventRecipient::Host)
        {
            const FGuid HostID = EntitySubsystem ? EntitySubsystem->GetHostID() : FGuid();
            const bool bIsLocalHost = HostID.IsValid() && HostID == LocalPlayerID;
            if (!bIsLocalHost)
            {
                UE_CLOG(FCrowdyRPC::IsRpcTraceEnabled(), LogCrowdyRPC, Verbose,
                    TEXT("[CrowdyRPC] host-only subsystem event '%s' ignored — this client is not the host."),
                    *Function->GetName());
                return;
            }
        }
        else // OwningClient on a non-spatial participant
        {
            if (!EntitySubsystem || !EntitySubsystem->IsLocallyOwned(Call.EntityID))
            {
                UE_CLOG(FCrowdyRPC::IsRpcTraceEnabled(), LogCrowdyRPC, Verbose,
                    TEXT("[CrowdyRPC] owner-only subsystem event '%s' ignored — this client does not own the participant."),
                    *Function->GetName());
                return;
            }
        }
    }

    UObject* Receiver = ResolveRpcReceiver(TargetParticipant, Function);
    if (!Receiver)
    {
        UE_LOG(LogCrowdyRPC, Warning,
            TEXT("[CrowdyEventRouter] RPC dropped — entity '%s' carries no '%s' to receive '%s'."),
            *TargetParticipant->GetName(), *GetNameSafe(Function->GetOwnerClass()), *Function->GetName());
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

UObject* UCrowdyEventRouter::ResolveStateContainer(UObject* Participant, const UClass* LayoutOwnerClass)
{
    if (!IsValid(Participant) || !LayoutOwnerClass)
    {
        return nullptr;
    }

    if (Participant->IsA(LayoutOwnerClass))
    {
        return Participant;
    }

    // Only an actor carries components; any other UObject resolves solely by IsA above. First matching component
    // wins, deterministically by GetComponents() order (same policy as ResolveRpcReceiver).
    if (AActor* Actor = Cast<AActor>(Participant))
    {
        for (UActorComponent* Component : Actor->GetComponents())
        {
            if (IsValid(Component) && Component->IsA(LayoutOwnerClass))
            {
                return Component;
            }
        }
    }

    return nullptr;
}

UObject* UCrowdyEventRouter::ResolveStateContainer(AActor* Actor, const UClass* LayoutOwnerClass)
{
    return ResolveStateContainer(static_cast<UObject*>(Actor), LayoutOwnerClass);
}

void UCrowdyEventRouter::DispatchStateDelta(const FCrowdyInboundEvent& Event, int32 RemainingAttempts)
{
    ensure(IsInGameThread());

    const FCrowdyStateDelta& Delta = Event.Payload.Get<FCrowdyStateDelta>();
    const FGuid LocalPlayerID = EntitySubsystem ? EntitySubsystem->GetLocalPlayerID() : FGuid();

    // Our own delta echoing back to us drop it (we already hold these values, we sent them). This covers a
    // spatial multicast echo AND a targeted owner-only delivery a host-owner addressed to its own entity, which
    // comes back with bTargetedDelivery set: the payload sender id is our own, so re-applying an identical value
    // and re-firing OnRep on the originator would be redundant. A legitimate host->owner correction carries the
    // HOST's id (never the receiving owner's), so it is never caught here. Unlike DispatchRpcCall (whose
    // owner/host-only body may intentionally re-run on its target), a state self-echo is always a no-op, so the
    // drop is not restricted to broadcasts.
    if (LocalPlayerID.IsValid() && Delta.SenderID == LocalPlayerID)
    {
        return;
    }

    UObject* TargetParticipant = EntitySubsystem ? EntitySubsystem->FindParticipant(Delta.EntityID) : nullptr;
    if (!TargetParticipant)
    {
        if (!Delta.EntityID.IsValid())
        {
            UE_LOG(LogCrowdyReplication, Verbose,
                TEXT("[CrowdyEventRouter] State delta ignored on receipt — no entity id."));
            return;
        }

        // The entity's spawn event may still be in flight; retry on later ticks (same budget as RPC).
        DeferEvent(Event, RemainingAttempts);
        return;
    }

    const FCrowdyRepLayout* Layout = AutoRegistry ? AutoRegistry->FindRepLayout(TargetParticipant->GetClass()) : nullptr;
    if (!Layout)
    {
        UE_LOG(LogCrowdyReplication, Warning,
            TEXT("[CrowdyEventRouter] State delta dropped — no rep layout for entity '%s' (ClassID=%lld); class has no CrowdyState properties or is not loaded."),
            *TargetParticipant->GetName(), Delta.ClassID);
        return;
    }

    // Coarse class guard before touching the blob: the sender's class id must match the resolved entity's. A
    // mismatch means sender and receiver disagree on the entity's class (drift/forgery); the LayoutHash guard
    // in Decode is the fine positional guard, this is the cheap early-out. (Both travel per FCrowdyStateDelta.)
    const FCrowdyClassID LocalClassID = UCrowdyClassRegistry::Get()->GetID(TargetParticipant->GetClass());
    if (static_cast<FCrowdyClassID>(Delta.ClassID) != LocalClassID)
    {
        UE_LOG(LogCrowdyReplication, Warning,
            TEXT("[CrowdyEventRouter] State delta dropped — ClassID mismatch for entity '%s' (delta=%lld local=%u)."),
            *TargetParticipant->GetName(), Delta.ClassID, LocalClassID);
        return;
    }

    // Host precedence by convention (NOT enforced): we are the authority for the entities we own. A delta for
    // an entity we own is applied only when it is HostSourced (a host correction); a foreign non-host delta is
    // dropped before touching the blob. (Our own echo including a targeted owner-only self-delivery was
    // already dropped above by SenderID.) A HostSourced delta falls through to apply and is then adopted (below)
    // so our own next diff does not revert the host's values.
    const bool bWeOwnTarget = EntitySubsystem && EntitySubsystem->IsLocallyOwned(Delta.EntityID);
    if (bWeOwnTarget)
    {
        const bool bHostSourced = (Delta.Flags & CrowdyStateDeltaFlags::HostSourced) != 0;
        if (!bHostSourced)
        {
            UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
                TEXT("[CrowdyState] dropped foreign non-host delta for owned entity %s (sender=%s)."),
                *Delta.EntityID.ToString(), *Delta.SenderID.ToString());
            return;
        }

        // HostOverride backstop: even a HostSourced correction is dropped if THIS entity is authored OwnerOnly (only
        // the owner may change it). The owner self-heals on its next diff/keyframe. Policy is authored config identical
        // on every client, so this matches the sender's own send-side check; a missing component -> Allow (apply). The
        // backstop only applies to actor participants; a non-actor participant carries no entity component and defaults
        // to Allow, matching the sender's missing-component send-side default.
        if (AActor* AsActor = Cast<AActor>(TargetParticipant))
        {
            if (const UCrowdyEntityComponent* Comp = AsActor->FindComponentByClass<UCrowdyEntityComponent>())
            {
                if (Comp->GetHostOverridePolicy() == ECrowdyHostOverride::OwnerOnly)
                {
                    UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
                        TEXT("[CrowdyState] dropped HostSourced delta for OwnerOnly entity %s (host override disallowed)."),
                        *Delta.EntityID.ToString());
                    return;
                }
            }
        }
    }
    else if (EntitySubsystem)
    {
        // World (host-owned) entities have no owner, so the bWeOwnTarget gate above never covers them (IsLocallyOwned
        // is false for a HostOwned record on every client). Only the host may write world state: drop a non-HostSourced
        // delta for a locally HostOwned entity. This closes the vector the Phase C untracked one-shot push opens (any
        // client could otherwise write a shared world entity). Still precedence-by-convention (a forged HostSourced from
        // a foreign client is honored) cheat-sensitive state belongs in Game Models, not on this unenforced view plane.
        if (const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(Delta.EntityID))
        {
            const bool bHostSourced = (Delta.Flags & CrowdyStateDeltaFlags::HostSourced) != 0;
            if (Record->Role == ECrowdyRole::HostOwned && !bHostSourced)
            {
                UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
                    TEXT("[CrowdyState] dropped non-host delta for host-owned world entity %s (sender=%s)."),
                    *Delta.EntityID.ToString(), *Delta.SenderID.ToString());
                return;
            }
        }
    }

    UObject* Container = ResolveStateContainer(TargetParticipant, Layout->OwnerClass.Get());
    if (!Container)
    {
        UE_LOG(LogCrowdyReplication, Warning,
            TEXT("[CrowdyEventRouter] State delta dropped — entity '%s' carries no '%s' to apply to."),
            *TargetParticipant->GetName(), *GetNameSafe(Layout->OwnerClass.Get()));
        return;
    }

    // The codec writes each present value into Container via ContainerPtrToValuePtr, guarding the LayoutHash
    // (drop on mismatch, target untouched) and every untrusted read. ChangedIndices lists the applied slots.
    TArray<int32> ChangedIndices;
    if (!FCrowdyStateCodec::Decode(*Layout, Delta.LayoutHash, Delta.Blob, Container, ChangedIndices))
    {
        // Decode already logged the precise reason (hash mismatch, bad version/selector, truncation, ...).
        return;
    }

    UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Log,
        TEXT("[CrowdyState] recv entity=%s sender=%s changed=%d bytes=%d %s"),
        *Delta.EntityID.ToString(), *Delta.SenderID.ToString(), ChangedIndices.Num(), Delta.Blob.Num(),
        Event.bTargetedDelivery ? TEXT("(targeted)") : TEXT("(broadcast)"));

    // Fire each changed property's parameterless CrowdyOnRep on the container. OnRep bindings were validated
    // at discovery (invalid ones cleared to NAME_None), so this only ever calls parameterless notifies; the
    // NumParms guard is defense-in-depth for a baked layout whose function drifted. No "previous value" here.
    for (int32 Index : ChangedIndices)
    {
        if (!Layout->Properties.IsValidIndex(Index))
        {
            continue;
        }

        const FName OnRepName = Layout->Properties[Index].OnRepFunctionName;
        if (OnRepName == NAME_None)
        {
            continue;
        }

        if (UFunction* OnRepFn = Container->FindFunction(OnRepName))
        {
            if (OnRepFn->NumParms == 0)
            {
                Container->ProcessEvent(OnRepFn, nullptr);
            }
        }
    }

    // A host correction for an entity WE own has now been applied; adopt the applied values into the owner's
    // shadow so the replicator's next diff sees them as already-sent and does not re-emit a revert. Best-effort:
    // a null / not-tracked replicator simply skips. bWeOwnTarget here already implies HostSourced (the gate
    // above dropped a non-host delta for an owned entity).
    if (bWeOwnTarget)
    {
        if (UCrowdyStateReplicator* Replicator = ResolveStateReplicator())
        {
            Replicator->AdoptHostValues(Delta.EntityID, *Layout, ChangedIndices, Container);
        }
    }
}

UCrowdyStateReplicator* UCrowdyEventRouter::ResolveStateReplicator() const
{
    if (StateReplicatorForTest)
    {
        return StateReplicatorForTest;
    }
    const UWorld* World = GetWorld();
    return World ? World->GetSubsystem<UCrowdyStateReplicator>() : nullptr;
}

void UCrowdyEventRouter::ReceiveLoopbackCall(const FCrowdyRpcCall& Call)
{
    ensure(IsInGameThread());

    // Replay a call this client just sent through its own receive path so a single client can
    // exercise serialize -> resolve -> dispatch without a second client. Marked as a targeted
    // delivery so it runs exactly once past the broadcast echo-drop and the owner-only guard
    // and never re-broadcasts, which is what bounds the loopback.
    FCrowdyInboundEvent Event;
    Event.Payload  = FInstancedStruct::Make(Call);
    Event.bTargetedDelivery = true;
    Event.Target   = ECrowdyTarget::Everyone;
    Event.SenderID = FGuid();
    Event.TargetID = FGuid();

    DispatchRpcCall(Event, CrowdyRpcMaxSpawnWaitAttempts);
}

void UCrowdyEventRouter::ReceiveLoopbackStateDelta(const FCrowdyStateDelta& Delta)
{
    ensure(IsInGameThread());

    // Replay a state delta this client just sent onto its own local mirror entity (see
    // UCrowdyStateReplicator::GetOrCreateLoopbackMirror), so a single client can exercise decode ->
    // OnRep -> the receive-side gates without a second client. bTargetedDelivery is set purely for
    // parity with ReceiveLoopbackCall; DispatchStateDelta only uses it for a trace-log cosmetic. The
    // real de-duplication is Delta.SenderID, which the caller has already cleared to FGuid() so this
    // never collides with the self-echo drop (that check only fires when SenderID == LocalPlayerID).
    FCrowdyInboundEvent Event;
    Event.Payload = FInstancedStruct::Make(Delta);
    Event.bTargetedDelivery = true;
    Event.Target = ECrowdyTarget::Everyone;
    Event.SenderID = FGuid();
    Event.TargetID = FGuid();

    DispatchStateDelta(Event, CrowdyStateMaxSpawnWaitAttempts);
}

void UCrowdyEventRouter::ReceiveChannelRpcCall(const FCrowdyRpcCall& Call)
{
    // A channel delivery is a multicast equivalent not a targeted single-actor send so the
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

void UCrowdyEventRouter::ReceiveChannelStateDelta(const FCrowdyStateDelta& Delta)
{
    // A channel state delta is a multicast equivalent, not a targeted single-actor send, so the broadcast
    // echo-drop (by SenderID) and the receive-side authority gates all apply exactly as for a spatial
    // Multicast. The originating client's id rides the payload, so the channel's wire UUID is not needed.
    FCrowdyInboundEvent Event;
    Event.Payload  = FInstancedStruct::Make(Delta);
    Event.bTargetedDelivery = false;
    Event.SenderID = Delta.SenderID;
    Event.Target   = ECrowdyTarget::Everyone;
    Event.TargetID = FGuid();

    // Mpsc queue: safe to enqueue off the game thread; Tick drains it through DispatchStateDelta.
    EventQueue.Enqueue(MoveTemp(Event));
}

void UCrowdyEventRouter::DeferEvent(const FCrowdyInboundEvent& Event, int32 RemainingAttempts)
{
    if (RemainingAttempts <= 1)
    {
        const UScriptStruct* PayloadType = Event.Payload.GetScriptStruct();
        FGuid EntityID;
        if (PayloadType == FCrowdyRpcCall::StaticStruct())
        {
            EntityID = Event.Payload.Get<FCrowdyRpcCall>().EntityID;
        }
        else if (PayloadType == FCrowdyStateDelta::StaticStruct())
        {
            EntityID = Event.Payload.Get<FCrowdyStateDelta>().EntityID;
        }

        UE_LOG(LogCrowdyReplication, Warning,
            TEXT("[CrowdyEventRouter] Deferred event for entity %s dropped — entity never spawned within the wait budget."),
            *EntityID.ToString());
        return;
    }

    DeferredEvents.Add({ Event, RemainingAttempts - 1 });
}

void UCrowdyEventRouter::RetryDeferredEvents()
{
    if (DeferredEvents.IsEmpty())
        return;

    // Swap the pending set out first so events that still can't resolve re-defer into a
    // fresh list for the next tick instead of looping within this one.
    TArray<FCrowdyDeferredEvent> Pending = MoveTemp(DeferredEvents);
    DeferredEvents.Reset();

    for (const FCrowdyDeferredEvent& Deferred : Pending)
    {
        const UScriptStruct* PayloadType = Deferred.Event.Payload.GetScriptStruct();
        if (PayloadType == FCrowdyRpcCall::StaticStruct())
        {
            DispatchRpcCall(Deferred.Event, Deferred.RemainingAttempts);
        }
        else if (PayloadType == FCrowdyStateDelta::StaticStruct())
        {
            DispatchStateDelta(Deferred.Event, Deferred.RemainingAttempts);
        }
        // Unknown payloads are never enqueued, so no else branch is needed.
    }
}

void UCrowdyEventRouter::Tick(float DeltaTime)
{
    // Drain every queued event; DispatchEvent routes RPC (FCrowdyRpcCall) and state (FCrowdyStateDelta).
    FCrowdyInboundEvent Event;
    while (EventQueue.Dequeue(Event))
    {
        DispatchEvent(Event);
    }

    RetryDeferredEvents();
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
    // never reach this router FCrowdyServiceRegistry routes them to their
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
