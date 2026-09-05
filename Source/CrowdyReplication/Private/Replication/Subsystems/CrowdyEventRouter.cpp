// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "CrowdyReplicationLog.h"
#include "Components/ActorComponent.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/RPC/FCrowdyEventParams.h"
#include "Replication/RPC/FCrowdyRpcCall.h"
#include "Replication/RPC/ICrowdyEntityEventHandler.h"
#include "Replication/RPC/ICrowdyEventSource.h"
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
#include "Templates/UnrealTemplate.h"   // TGuardValue (the state-dispatch re-entrancy guard)
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

	/**
	 * Hands a registration slot to a newcomer under the world-travel rule.
	 *
	 * Three parts, and each exists for the same reason: two worlds are alive at once while travelling between
	 * levels, and the world being left runs its teardown AFTER the world being entered has registered.
	 *   - The newest registration wins, so the arriving world's holder takes the slot.
	 *   - A holder that has been displaced never takes the slot back, because registration is reached from
	 *     more than one place in its life and a departing world can still run one of them.
	 *   - Displaced entries are pruned as they die, so a long session cannot accumulate them.
	 *
	 * SlotLabel names the slot in the log lines; bTrace is the caller's own trace switch, so each slot
	 * stays on the log channel its area already uses. Written against the slot rather than against one
	 * interface so the rule stays stated once however many slots a later change gives this router.
	 */
	template <typename SlotInterface>
	void ClaimProviderSlot(TScriptInterface<SlotInterface>& Slot, TArray<TWeakObjectPtr<UObject>>& Displaced,
		const TScriptInterface<SlotInterface>& Incoming, const TCHAR* SlotLabel, const bool bTrace)
	{
		UObject* IncomingObject = Incoming.GetObject();
		if (!IsValid(IncomingObject) || !Incoming.GetInterface())
		{
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("[CrowdyEventRouter] %s ignored - it is null or does not implement the interface."),
				SlotLabel);
			return;
		}

		// Only live entries are worth keeping, so a session cannot accumulate them: once a displaced world is
		// collected it can no longer register anything and needs no record.
		Displaced.RemoveAll(
			[](const TWeakObjectPtr<UObject>& Entry) { return !Entry.IsValid(); });

		const bool bAlreadyDisplaced = Displaced.ContainsByPredicate(
			[IncomingObject](const TWeakObjectPtr<UObject>& Entry) { return Entry.Get() == IncomingObject; });
		if (bAlreadyDisplaced)
		{
			UE_LOG(LogCrowdyReplication, Warning,
				TEXT("[CrowdyEventRouter] %s '%s' was displaced by a newer one; refusing to re-register it."),
				SlotLabel, *IncomingObject->GetName());
			return;
		}

		UObject* PreviousObject = Slot.GetObject();
		if (PreviousObject == IncomingObject)
		{
			return;
		}

		if (PreviousObject)
		{
			Displaced.Add(PreviousObject);
			UE_CLOG(bTrace, LogCrowdyReplication, Log,
				TEXT("[CrowdyEventRouter] %s '%s' took over from '%s'."),
				SlotLabel, *IncomingObject->GetName(), *PreviousObject->GetName());
		}

		Slot = Incoming;
	}

	/**
	 * Releases a registration slot, but only on behalf of whoever actually holds it.
	 *
	 * A departing world releasing after the arriving world has registered is the ordinary case, not a rare
	 * one, and clearing the slot then would leave everything addressed to the live world's entities with
	 * nowhere to go.
	 */
	template <typename SlotInterface>
	void ReleaseProviderSlot(TScriptInterface<SlotInterface>& Slot,
		const TScriptInterface<SlotInterface>& Outgoing, const TCHAR* SlotLabel, const bool bTrace)
	{
		UObject* OutgoingObject = Outgoing.GetObject();
		if (!OutgoingObject || Slot.GetObject() != OutgoingObject)
		{
			UE_CLOG(bTrace, LogCrowdyReplication, Verbose,
				TEXT("[CrowdyEventRouter] %s release ignored - '%s' does not hold the registration."),
				SlotLabel, *GetNameSafe(OutgoingObject));
			return;
		}

		Slot = nullptr;
	}

	// DestroyValue every value initialized into a state scratch buffer, through the properties snapshotted
	// when it was built rather than through a layout: the registry frees and rebuilds its layouts on a
	// rescan, while these UClass-owned FProperty* survive one.
	//
	// A buffer whose class has gone away is ABANDONED instead: its snapshotted properties belong to that
	// class, and a Live Coding reload that reinstances a class leaves them dangling, so destroying through
	// them would dereference freed memory. Leaking the values of a class that no longer exists is bounded
	// and editor-only, which is the same trade the send plane's shadow buffers already make.
	// Named for this buffer specifically: an identically named helper in another translation unit of this
	// module would be merged into one unity translation unit and redefine its twin.
	void DestroyStateScratchValues(FCrowdyStateScratchContainer& Scratch)
	{
		if (!Scratch.Data)
		{
			return;
		}

		if (!Scratch.OwnerClass.IsValid())
		{
			UE_LOG(LogCrowdyReplication, Verbose,
				TEXT("[CrowdyEventRouter] State scratch buffer abandoned without destroying its values: its class is gone, so the properties describing them are too."));
			return;
		}

		for (const FProperty* Property : Scratch.InitializedProps)
		{
			if (Property)
			{
				Property->DestroyValue_InContainer(Scratch.Data);
			}
		}
	}

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

	// The entity an event held on the wait queue is waiting for. Only RPC calls and state deltas are ever
	// held, so anything else answers with an invalid id rather than pretending to know. Takes the payload
	// itself, so a queued entry is read through the copy the queue owns rather than through a borrowed view.
	FGuid ResolveDeferredEntityID(const FInstancedStruct& Payload)
	{
		const UScriptStruct* PayloadType = Payload.GetScriptStruct();
		if (PayloadType == FCrowdyRpcCall::StaticStruct())
		{
			return Payload.Get<FCrowdyRpcCall>().EntityID;
		}
		if (PayloadType == FCrowdyStateDelta::StaticStruct())
		{
			return Payload.Get<FCrowdyStateDelta>().EntityID;
		}
		return FGuid();
	}

	// True for a call whose receiver is declared on a class that is itself a Crowdy event source, which is
	// how an entity with no actor declares its one-shot actions. Those get a wait measured in time rather
	// than in retries: an action lasts a fraction of a second, so one delivered long after it happened is
	// not late, it is wrong, and the entity it names may have died and respawned under the same id while
	// it waited. A call whose function does not resolve is left unclassified and keeps the ordinary wait.
	bool IsActionScopedCall(const UFunction* Function)
	{
		const UClass* OwnerClass = Function ? Function->GetOwnerClass() : nullptr;
		return OwnerClass && OwnerClass->ImplementsInterface(UCrowdyEventSource::StaticClass());
	}

	// True when no actor could ever run this call, so waiting for one cannot be what a call naming a
	// missing entity is waiting for. An actor participant resolves a receiver as itself or as one of its
	// own components (see ResolveRpcReceiver), so a function declared anywhere else is out of an actor's
	// reach however long the wait.
	//
	// The wait budget reads this. A call an arriving actor could still run must keep the ordinary spawn
	// wait: it is waiting for the actor rather than for a representation, and cutting that wait to a
	// fraction of a second throws away calls that would have landed.
	bool IsOutOfAnActorsReach(const UFunction* Function)
	{
		const UClass* OwnerClass = Function ? Function->GetOwnerClass() : nullptr;
		if (!OwnerClass)
		{
			return false;
		}

		return !OwnerClass->IsChildOf(AActor::StaticClass())
			&& !OwnerClass->IsChildOf(UActorComponent::StaticClass());
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

    if (Bridge->ServiceRegistry)
    {
        SubscribeToEventFallback(*Bridge->ServiceRegistry);
    }
}

void UCrowdyEventRouter::SubscribeToEventFallback(FCrowdyServiceRegistry& Registry)
{
    // The router is the fallback for every event type nothing else Handles (entity spawn/destroy,
    // the Game Model's three carriers, and any other claimed event id all take Handle keys and are
    // never seen here).
    EventFallbackSubscription = Registry.SubscribeToAllPayloads(
        ECrowdyPayloadCategory::Event,
        { ECrowdySubscriptionRole::Fallback, /*bRequiresExclusiveHandling*/ false, TEXT("CrowdyEventRouter") },
        [this](const FCrowdyDelivery& Delivery) { HandleDelivery(Delivery); });
}

void UCrowdyEventRouter::Deinitialize()
{
    // Releasing the handle stops any further delivery to this subscriber, including later in the same
    // fan-out, so this needs no explicit registry lookup. See FCrowdyDelivery for the threading rules.
    EventFallbackSubscription.Release();
    Bridge = nullptr;

    DeferredEvents.Empty();
    AutoRegistry = nullptr;

    // This router is going away with its world, so its own registration goes with it. Only the slot is
    // cleared: the displaced list stays, so a subscriber this router already replaced still cannot claim it
    // during the rest of the teardown.
    EntitySubscriber = nullptr;

    // Freed here rather than left to the router's own destruction, so the values initialized into them are
    // destroyed while the classes describing them are still around to describe them.
    StateScratchContainers.Empty();
    StateDecodeScratches.Empty();

    Super::Deinitialize();
}

void UCrowdyEventRouter::DispatchEvent(const FCrowdyInboundEvent& Event)
{
	ensure(IsInGameThread());

    const UScriptStruct* StructType = Event.Payload ? Event.Payload->GetScriptStruct() : nullptr;
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

    const FCrowdyRpcCall& Call = Event.Payload->Get<FCrowdyRpcCall>();
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

        // Both the handoff below and the wait budget need the function this call names, so resolve it
        // once here. A function that does not resolve is NOT dropped at this point: that decision belongs
        // to the path that has a participant, and this one must behave the same as it always has.
        UFunction* PendingFunction = AutoRegistry
            ? AutoRegistry->ResolveFunction(Call.ClassID, Call.FunctionID)
            : nullptr;

        // No actor-side participant holds this id, but an entity that lives outside that registry still
        // might. Ask the registered subscriber before queueing: such an entity never registers there, so the
        // call would otherwise wait out its whole budget and then drop for an entity that was present the
        // entire time.
        if (DispatchCallToSubscriber(Call, PendingFunction))
        {
            return;
        }

        // The entity's spawn event may still be in flight; retry on later ticks.
        //
        // A function the registered subscriber has a handler for takes the same short budget as a call
        // declared on an event source, but only while no actor could run it either. Both describe a change
        // to what an entity looks like rather than a fact about it, and such a change is only worth making
        // while it is still current: the entity a late one names may have died and respawned under the
        // same id while it waited. A call an arriving ACTOR could run is not that case at all. It is
        // waiting for the actor, which is exactly what the ordinary spawn wait is for, and judging it on a
        // fraction of a second evicts calls that would have landed on the actor a moment later.
        //
        // The subscriber is asked about the FUNCTION alone, which is all there is to ask about while the
        // target has not arrived.
        const bool bShortWait = IsActionScopedCall(PendingFunction)
            || (HasSubscriberEventHandler(PendingFunction) && IsOutOfAnActorsReach(PendingFunction));
        const double DeferBudget = bShortWait ? CrowdyDeferQueue::ActionBudgetSeconds : 0.0;
        DeferEvent(Event, RemainingAttempts, DeferBudget);
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
        // The entity has a participant, but nothing on that participant declares this function, so it is
        // not a home for the call. Something else may still hold the same entity as data: an object
        // enrolled as a participant for another plane leaves the entity's events to a subscriber. Offer it
        // there before dropping, because presence is not capability, and a participant that cannot run the
        // call must not displace one that can.
        //
        // Deliberately BELOW the owner/host guards above rather than beside the participant lookup. Those
        // guards settle who may run this call, from the entity's own record and the function's recipient,
        // and a fall-through placed over them would hand a subscriber exactly the calls they refuse.
        //
        // Not deferred either way: the entity is already registered, so waiting cannot give this
        // participant a function it does not declare.
        if (DispatchCallToSubscriber(Call, Function))
        {
            return;
        }

        // Counted whether or not it is reported, and the count is taken outside the log statement so it
        // survives a suppressed category: a rate-limited line and no traffic at all look identical, and
        // the counter is what tells them apart.
        const bool bReportDrop = CountUndeliverableCall();
        UE_CLOG(bReportDrop, LogCrowdyRPC, Warning,
            TEXT("[CrowdyEventRouter] RPC dropped - entity '%s' carries no '%s' to receive '%s', and no subscriber holds it. Further drops on this path are reported at most once a second."),
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

bool UCrowdyEventRouter::DispatchCallToSubscriber(const FCrowdyRpcCall& Call, UFunction* Function)
{
    ICrowdyEntitySubscriber* Subscriber = EntitySubscriber.GetInterface();
    if (!Subscriber || !IsValid(EntitySubscriber.GetObject()))
    {
        return false;
    }

    if (!Function)
    {
        // There is nothing to hand the subscriber, and no verdict this branch can reach: the declaring
        // class may simply not be loaded yet. Not a subscriber decision at all, so it is taken before the
        // subscriber is consulted; the call stays on the wait path, which reports an unresolvable function
        // the way it already does.
        return false;
    }

    if (!Subscriber->IsEntityKnown(Call.EntityID))
    {
        // Not this subscriber's id. It may still be an entity on its way in, so the caller goes on waiting
        // exactly as it did before a subscriber existed. This one question settles wait against refuse, and
        // nothing below may reopen it: past here every exit is final.
        return false;
    }

    const FCrowdyFnInfo Info = FCrowdyRPC::GetFnInfo(Function);

    // Owner-only and host-only calls reach their single recipient over the single-actor transport, which
    // addresses an actor. The subscriber has just claimed this entity, meaning it holds it as data with no
    // actor of its own, so a call carrying such a recipient is misuse or forgery either way, and running it
    // on every client that received the broadcast is the one outcome that must not happen. Checked before
    // anything is resolved or decoded, because it depends only on the function and on that claim, both of
    // which are already settled. The caller that arrives here with a participant has applied its own
    // owner/host guards first, so this never reopens a decision that path already made.
    if (Info.Recipient == ECrowdyEventRecipient::OwningClient
        || Info.Recipient == ECrowdyEventRecipient::Host)
    {
        const bool bReportDrop = CountUndeliverableCall();
        UE_CLOG(bReportDrop, LogCrowdyRPC, Warning,
            TEXT("[CrowdyEventRouter] RPC dropped - '%s' is owner/host-only, which an entity with no actor cannot be sent. Further drops on this path are reported at most once a second."),
            *Function->GetName());
        return true;
    }

    // No class identity is compared on this route, and saying so plainly beats leaving a comparison that
    // reads like a guard. The function was resolved from the call's (ClassID, FunctionID) against a
    // registry keyed by the class that DECLARES each function, so an identity this build does not know
    // resolves no function at all and never reaches here, and one that does resolve already agrees with
    // the key it was found under. Comparing the declaring class's id back against the call's could only
    // restate that key.
    //
    // What IS checked, so the omission is not read as an absence of checks: the object the subscriber
    // offers below is checked against the declaring class before anything is invoked on it, and whether
    // this entity is the KIND the event was declared for is the subscriber's own refusal to make, since
    // only the subscriber knows what it recorded the entity as (see ICrowdyEntityEventHandler).
    UObject* Receiver = Subscriber->ResolveReceiver(Call.EntityID, Function);
    if (!Receiver)
    {
        // No object here declares the function, which is the ordinary case for an event declared on the
        // class the entity's owner runs as a real object: the owner runs the body and this client holds
        // the entity as data. There is nothing to invoke, so the call is offered as values instead.
        DispatchCallToEventHandler(Call, Function, Info);
        return true;
    }

    // What the actor path gets for free from resolving its own receiver, and this path does not: the
    // subscriber chose this object and nothing so far has checked it against the function.
    const UClass* OwnerClass = Function->GetOwnerClass();
    if (!IsValid(Receiver) || !OwnerClass || !Receiver->IsA(OwnerClass))
    {
        const bool bReportDrop = CountUndeliverableCall();
        UE_CLOG(bReportDrop, LogCrowdyRPC, Warning,
            TEXT("[CrowdyEventRouter] RPC dropped - the receiver offered for entity %s is a '%s', which does not declare '%s'. Further drops on this path are reported at most once a second."),
            *Call.EntityID.ToString(), *GetNameSafe(IsValid(Receiver) ? Receiver->GetClass() : nullptr),
            *Function->GetName());
        return true;
    }

    UE_CLOG(FCrowdyRPC::IsRpcTraceEnabled(), LogCrowdyRPC, Log,
        TEXT("[CrowdyRPC] recv %s::%s entity=%s sender=%s (entity subscriber) bytes=%d"),
        *GetNameSafe(Receiver->GetClass()), *Function->GetName(),
        *Call.EntityID.ToString(), *Call.SenderID.ToString(), Call.ParamBlob.Num());

    FCrowdyRPC::FScopedEntityContext EntityContext(EntitySubsystem);
    FCrowdyRPC::ApplyCall(Receiver, Function, Info, Call);
    return true;
}

void UCrowdyEventRouter::DispatchCallToEventHandler(const FCrowdyRpcCall& Call, UFunction* Function,
    const FCrowdyFnInfo& Info)
{
    ICrowdyEntityEventHandler* Handler = Cast<ICrowdyEntityEventHandler>(EntitySubscriber.GetObject());
    if (!Handler || !Handler->HasEntityEventHandler(Function))
    {
        ReportUnhandledSubscriberEvent(Function);
        return;
    }

    // Charged before the parameter frame is built, never after. Decoding is the expensive half of
    // receiving a call, so an allowance spent once the values already exist bounds the work after it has
    // been done, and both the entity id and the number of calls naming it were chosen by whoever sent
    // them. A receiver that meters nothing accepts and nothing changes for it.
    if (!Handler->ChargeEntityEventBudget(Call.EntityID, Function))
    {
        ++BudgetRefusedCallCount;
        UE_CLOG(FCrowdyRPC::IsRpcTraceEnabled(), LogCrowdyRPC, Verbose,
            TEXT("[CrowdyEventRouter] RPC dropped for entity %s - its inbound allowance is spent."),
            *Call.EntityID.ToString());
        return;
    }

    UE_CLOG(FCrowdyRPC::IsRpcTraceEnabled(), LogCrowdyRPC, Log,
        TEXT("[CrowdyRPC] recv %s entity=%s sender=%s (entity subscriber, applied as data) bytes=%d"),
        *Function->GetName(), *Call.EntityID.ToString(), *Call.SenderID.ToString(), Call.ParamBlob.Num());

    // The entity scope lets an object-reference parameter resolve against this world while the call is
    // decoded, exactly as on the path that invokes a body.
    FCrowdyRPC::FScopedEntityContext EntityContext(EntitySubsystem);

    const FGuid EntityID = Call.EntityID;
    const FGuid SenderID = Call.SenderID;
    FCrowdyRPC::DecodeCall(Function, Info, Call,
        [Handler, &EntityID, &SenderID, Function](const FCrowdyEventParams& Params)
        {
            Handler->HandleEntityEvent(EntityID, SenderID, Function, Params);
        });
}

void UCrowdyEventRouter::ReportUnhandledSubscriberEvent(const UFunction* Function)
{
    ++UnhandledSubscriberEventCount;

    // Once per function, never once per event. The cause is a single missing registration, and a crowd
    // means it is hit once per entity per event, so a line each would bury the one sentence that says what
    // to register. Counting every drop alongside it is what keeps a throttled report from looking like no
    // traffic at all.
    bool bAlreadyReported = false;
    ReportedUnhandledEventFunctions.Add(FObjectKey(Function), &bAlreadyReported);
    if (bAlreadyReported)
    {
        return;
    }

    ++UnhandledSubscriberEventReportCount;
    UE_LOG(LogCrowdyRPC, Warning,
        TEXT("[CrowdyEventRouter] '%s' reached an entity this client holds as data, and nothing is registered to apply it. ")
        TEXT("Its owner ran the body on their own actor; here there is no instance to run it on, so register what the ")
        TEXT("event should do to the representation instead. Reported once for this event."),
        *GetNameSafe(Function));
}

bool UCrowdyEventRouter::CountUndeliverableCall()
{
    ++UndeliverableCallCount;

    const double Now = FPlatformTime::Seconds();
    if (Now < NextUndeliverableCallReportSeconds)
    {
        return false;
    }

    NextUndeliverableCallReportSeconds = Now + 1.0;
    ++UndeliverableCallReportCount;
    return true;
}

bool UCrowdyEventRouter::HasSubscriberEventHandler(const UFunction* Function) const
{
    UObject* SubscriberObject = EntitySubscriber.GetObject();
    if (!Function || !IsValid(SubscriberObject))
    {
        return false;
    }

    const ICrowdyEntityEventHandler* Handler = Cast<ICrowdyEntityEventHandler>(SubscriberObject);
    return Handler && Handler->HasEntityEventHandler(Function);
}

void UCrowdyEventRouter::RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber> Subscriber)
{
    ClaimProviderSlot(EntitySubscriber, DisplacedEntitySubscribers, Subscriber,
        TEXT("Entity subscriber"), CrowdyReplicationTrace::Entity());
}

void UCrowdyEventRouter::UnregisterEntitySubscriber(const TScriptInterface<ICrowdyEntitySubscriber>& Subscriber)
{
    ReleaseProviderSlot(EntitySubscriber, Subscriber,
        TEXT("Entity subscriber"), CrowdyReplicationTrace::Entity());
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

FCrowdyStateScratchContainer::~FCrowdyStateScratchContainer()
{
	DestroyStateScratchValues(*this);

	if (Data)
	{
		FMemory::Free(Data);
		Data = nullptr;
	}
}

FCrowdyStateScratchContainer* UCrowdyEventRouter::ResolveStateScratch(const UClass* Class, const FCrowdyRepLayout& Layout)
{
	if (!Class)
	{
		return nullptr;
	}

	TUniquePtr<FCrowdyStateScratchContainer>& Slot = StateScratchContainers.FindOrAdd(Class);

	// A buffer whose layout hash has moved describes a different set of slots, and one whose class was
	// reinstanced holds properties that no longer describe anything. Either way it is torn down and rebuilt
	// rather than decoded into; the destructor abandons a dead class's values rather than dereferencing them.
	if (Slot.IsValid() && (Slot->LayoutHash != Layout.LayoutHash || !Slot->OwnerClass.IsValid()))
	{
		Slot.Reset();
	}

	if (Slot.IsValid())
	{
		return Slot.Get();
	}

	// Shaped like an instance of the class, because the codec addresses a value as this base plus the
	// property's own offset within its class. No instance is constructed: constructing one would build
	// components and subobjects, and an actor class constructed with no world is a trap.
	const int32 Size = Class->GetPropertiesSize();
	if (Size <= 0)
	{
		return nullptr;
	}

	TUniquePtr<FCrowdyStateScratchContainer> Built = MakeUnique<FCrowdyStateScratchContainer>();
	Built->OwnerClass = Class;
	Built->LayoutHash = Layout.LayoutHash;
	// Once per class rather than per delta: see the member's comment for what deriving it actually costs.
	Built->ClassID = UCrowdyClassRegistry::Get()->GetID(Class);
	Built->Size = Size;

	// Zeroed before anything is initialized, so a property this layout does not name is never read as
	// uninitialized memory and the padding between values is deterministic.
	Built->Data = static_cast<uint8*>(FMemory::Malloc(Size, FMath::Max(1, Class->GetMinAlignment())));
	FMemory::Memzero(Built->Data, Size);

	// Only the properties the layout names are initialized, which is what lets a plain allocation stand in
	// for an instance: a CrowdyState layout is restricted to POD leaves and plain USTRUCTs and rejects object
	// references, interfaces, delegates and containers, so nothing in it points at something that has to be
	// constructed to be readable. They ARE initialized rather than left zeroed because a leaf may own heap,
	// and a value decoded over uninitialized memory would be read as a garbage allocation.
	Built->InitializedProps.Reserve(Layout.Properties.Num());
	for (const FCrowdyRepProperty& RepProperty : Layout.Properties)
	{
		if (!RepProperty.Property)
		{
			continue;
		}

		RepProperty.Property->InitializeValue_InContainer(Built->Data);
		Built->InitializedProps.Add(RepProperty.Property);
	}

	UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Log,
		TEXT("[CrowdyEventRouter] State scratch built for %s: %d bytes, %d of %d layout slots initialized."),
		*Class->GetName(), Size, Built->InitializedProps.Num(), Layout.Properties.Num());

	Slot = MoveTemp(Built);
	return Slot.Get();
}

FCrowdyStateDecodeScratch* UCrowdyEventRouter::ResolveDecodeScratch(const FCrowdyRepLayout& Layout)
{
	const UClass* Owner = Layout.OwnerClass.Get();
	if (!Owner)
	{
		return nullptr;
	}

	// Keyed by the class the layout's properties belong to, which is what decides the block's shape. The block
	// rebuilds itself when the layout hash moves under it, so a stale entry is never decoded into.
	TUniquePtr<FCrowdyStateDecodeScratch>& Slot = StateDecodeScratches.FindOrAdd(Owner);
	if (!Slot.IsValid())
	{
		Slot = MakeUnique<FCrowdyStateDecodeScratch>();
	}

	// Built here rather than inside the decode, so the class resolved just above is the only one this path
	// resolves: the block validates itself against it and the decode then finds a block that already fits.
	Slot->EnsureForLayout(Layout, Owner);
	return Slot.Get();
}

bool UCrowdyEventRouter::DispatchStateDeltaToSubscriber(const FCrowdyStateDelta& Delta)
{
	ICrowdyEntitySubscriber* Subscriber = EntitySubscriber.GetInterface();
	if (!Subscriber || !IsValid(EntitySubscriber.GetObject()))
	{
		return false;
	}

	// Whether the id is the subscriber's at all, asked on its own and read by nothing else. Not answered from
	// the class below, and the class is not answered from this: an entity may legitimately be held with no
	// class (a position with nothing declared for it), and folding the two together would turn every delta
	// for such an entity into a wait that can only time out, with a crowd of them evicting the deltas for
	// entities that genuinely are still arriving.
	if (!Subscriber->IsEntityKnown(Delta.EntityID))
	{
		++StateSubscriberStats.NotClaimed;
		return false;
	}

	++StateSubscriberStats.Claimed;

	// From here the id IS the subscriber's, so every exit below is final. Waiting could not help: nothing this
	// router is waiting for is going to make the delta applicable, and a crowd of entities re-deferring a
	// delta per change would fill the shared wait queue with work that can never resolve, evicting the events
	// for entities that genuinely are still spawning.

	// The class the values are read against, resolved locally when the entity was created and never taken off
	// the delta being judged. Null is ordinary and final: the entity is held, it simply has nothing declared
	// to decode into, so there is no layout and nothing to apply.
	const UClass* EntityClass = Subscriber->GetEntityClass(Delta.EntityID);
	if (!EntityClass)
	{
		++StateSubscriberStats.DroppedNoEntityClass;
		UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
			TEXT("[CrowdyState] delta refused for entity %s: it is held with no class recorded, so nothing declares what these bytes are."),
			*Delta.EntityID.ToString());
		return true;
	}

	// This replaces the participant path's ClassID guard, and it is a stronger check rather than a weaker one:
	// the class comes from what this client resolved locally for the entity, so a peer can only match the
	// class this client already decided the entity has. Nothing off the wire chooses the layout the bytes are
	// read against.
	// Resolved through the same registry call the participant path uses, so the layout a delta is read against
	// is identical on both paths and the cooked build assembles it from the baked table the same way.
	const FCrowdyRepLayout* Layout = AutoRegistry ? AutoRegistry->FindRepLayout(EntityClass) : nullptr;
	if (!Layout)
	{
		UE_CLOG(ShouldReportStateSubscriberDrop(), LogCrowdyReplication, Warning,
			TEXT("[CrowdyEventRouter] State delta dropped — no rep layout for entity %s (class %s has no CrowdyState properties or is not loaded). Further drops on this path are reported at most once a second."),
			*Delta.EntityID.ToString(), *EntityClass->GetName());
		++StateSubscriberStats.DroppedNoLayout;
		return true;
	}

	// Resolved before the class guard below, deliberately: the buffer is keyed by a class this client itself
	// decided the entity has, never by anything off the wire, so a peer cannot drive its creation. Doing it in
	// this order is what lets the guard compare against an id resolved once per class instead of rebuilding a
	// class path string and interning it on every delta.
	FCrowdyStateScratchContainer* Scratch = ResolveStateScratch(EntityClass, *Layout);
	if (!Scratch || !Scratch->Data)
	{
		UE_CLOG(ShouldReportStateSubscriberDrop(), LogCrowdyReplication, Warning,
			TEXT("[CrowdyEventRouter] State delta dropped — no decode buffer could be built for class %s."),
			*EntityClass->GetName());
		++StateSubscriberStats.DroppedNoScratch;
		return true;
	}

	if (static_cast<uint32>(Delta.ClassID) != Scratch->ClassID)
	{
		++StateSubscriberStats.DroppedClassIdMismatch;
		// Rate-limited rather than per delta: this is the shape a wrong or drifted sender produces for every
		// entity of a class at once, and at crowd scale a line per delta is its own denial of service.
		UE_CLOG(ShouldReportStateSubscriberDrop(), LogCrowdyReplication, Warning,
			TEXT("[CrowdyEventRouter] State delta dropped — ClassID mismatch for entity %s (delta=%lld local=%u for class %s). Further drops on this path are reported at most once a second."),
			*Delta.EntityID.ToString(), Delta.ClassID, Scratch->ClassID, *EntityClass->GetName());
		return true;
	}

	// The subscriber is handed the PRESENT set, not the changed set, and the difference is the whole reason this
	// buffer is safe to share. The codec reports a slot as changed only when the decoded value differs from
	// what the container already held; on a per-target container that means "this target's value moved", but on
	// this shared buffer it means "differs from whatever the PREVIOUS entity left here". An entity legitimately
	// sent the same value its predecessor happened to hold would then be reported as having been sent nothing,
	// and would keep whatever its fragment held forever, because the heartbeat's re-sends are suppressed the
	// same way. The present set is a fact about the delta alone, so it says what this entity was sent.
	//
	// Both arrays are members rather than locals so a crowd's worth of deltas reuses one allocation each. The
	// present set is handed to the subscriber by reference and read for as long as ApplyDecodedState runs, and
	// that call may dispatch another delta. The members are therefore claimed by the outermost dispatch only; a
	// re-entrant one takes these locals so it cannot write into the set its caller is still reading.
	TArray<int32> ReentrantChangedIndices;
	TArray<int32> ReentrantPresentIndices;
	const bool bReentrantDispatch = bDispatchingSubscriberStateDelta;
	TGuardValue<bool> DispatchGuard(bDispatchingSubscriberStateDelta, true);
	TArray<int32>& ChangedIndices = bReentrantDispatch ? ReentrantChangedIndices : StateDecodeChangedIndices;
	TArray<int32>& PresentIndices = bReentrantDispatch ? ReentrantPresentIndices : StateDecodePresentIndices;

	if (!FCrowdyStateCodec::Decode(*Layout, Delta.LayoutHash, Delta.Blob, Scratch->Data,
		ChangedIndices, &PresentIndices, ResolveDecodeScratch(*Layout)))
	{
		// Decode already logged the precise reason (hash mismatch, bad version/selector, truncation, ...). A
		// partial decode leaves values in the shared buffer, which is exactly why nothing is handed over here:
		// the subscriber would have no way to tell a delivered value from a leftover one.
		++StateSubscriberStats.DroppedDecodeFailed;
		return true;
	}

	UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Log,
		TEXT("[CrowdyState] recv entity=%s sender=%s delivered=%d bytes=%d (applied as data)"),
		*Delta.EntityID.ToString(), *Delta.SenderID.ToString(), PresentIndices.Num(), Delta.Blob.Num());

	// No CrowdyOnRep notify fires on this path, by design rather than by omission: a notify is a UFunction
	// call on an object, and an entity that receives its state as data has no object to call one on. A
	// subscriber that needs work to happen on a change does it here, driven by the indices it is handed.
	++StateSubscriberStats.Applied;
	StateSubscriberStats.DeliveredSlots += PresentIndices.Num();
	Subscriber->ApplyDecodedState(Delta.EntityID, *Layout, Scratch->Data, PresentIndices);
	return true;
}

bool UCrowdyEventRouter::ShouldReportStateSubscriberDrop()
{
	const double Now = FPlatformTime::Seconds();
	if (Now < NextStateSubscriberDropReportSeconds)
	{
		return false;
	}

	NextStateSubscriberDropReportSeconds = Now + 1.0;
	return true;
}

void UCrowdyEventRouter::DispatchStateDelta(const FCrowdyInboundEvent& Event, int32 RemainingAttempts)
{
    ensure(IsInGameThread());

    const FCrowdyStateDelta& Delta = Event.Payload->Get<FCrowdyStateDelta>();
    const FGuid LocalPlayerID = EntitySubsystem ? EntitySubsystem->GetLocalPlayerID() : FGuid();

    // Our own delta echoing back to us drop it (we already hold these values, we sent them). This covers a
    // spatial multicast echo AND a targeted owner-only delivery a host-owner addressed to its own entity, which
    // comes back with bTargetedDelivery set: the payload sender id is our own, so re-applying an identical value
    // would be redundant, and the originator already fired the notify for it on the send side
    // (UCrowdyStateReplicator). A legitimate host->owner correction carries the HOST's id (never the receiving
    // owner's), so it is never caught here. Unlike DispatchRpcCall (whose owner/host-only body may intentionally
    // re-run on its target), a state self-echo is always a no-op, so the drop is not restricted to broadcasts.
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

        // An entity with no object to apply to may still be one something else holds as data (a row in a
        // table rather than a spawned actor). Asked only after the registry lookup above has failed, so a
        // subscriber never displaces a participant that can actually hold this state. A participant whose
        // class carries no rep layout is not a home for state at all, so the layout check below falls through
        // here too rather than dropping the delta.
        if (DispatchStateDeltaToSubscriber(Delta))
        {
            return;
        }

        // The entity's spawn event may still be in flight; retry on later ticks (same budget as RPC).
        DeferEvent(Event, RemainingAttempts);
        return;
    }

    // Who may write this entity is settled before anything about where its state ends up, because the answer
    // does not depend on that: it comes from the entity's own record and the delta's flags. Deliberately above
    // the layout lookup, so it governs every destination below alike, the participant itself and the fragment
    // subscriber a participant with no layout falls through to. All three checks need a registered entity, which
    // the branch above has just established.
    //
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
            ++StateParticipantStats.DroppedNotHostSourcedForOwned;
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
                    ++StateParticipantStats.DroppedHostOverrideRefused;
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
                ++StateParticipantStats.DroppedNotHostSourcedForWorld;
                UE_CLOG(CrowdyReplicationTrace::State(), LogCrowdyReplication, Verbose,
                    TEXT("[CrowdyState] dropped non-host delta for host-owned world entity %s (sender=%s)."),
                    *Delta.EntityID.ToString(), *Delta.SenderID.ToString());
                return;
            }
        }
    }

    // The lookup already resolves the participant's wire class id to key its cache, so the coarse guard
    // below reads that answer instead of asking the registry for the same class a second time.
    FCrowdyClassID LocalClassID = CROWDY_INVALID_CLASS_ID;
    const FCrowdyRepLayout* Layout = AutoRegistry
        ? AutoRegistry->FindRepLayout(TargetParticipant->GetClass(), LocalClassID)
        : nullptr;
    if (!Layout)
    {
        // The entity has an object, but that object's class declares no CrowdyState properties, so it cannot
        // hold this delta. Something else may still hold the same entity as data (an avatar registered for
        // another plane leaves the entity's view state to a subscriber), so offer it before dropping. Not
        // deferred either way: the entity is already registered, so waiting cannot give this participant a
        // layout it does not have.
        if (DispatchStateDeltaToSubscriber(Delta))
        {
            return;
        }

        // Counted before the line is even considered, because the line is rate-limited and the count is not:
        // a suppressed line and no traffic at all look identical, and the counter is what tells them apart.
        ++StateParticipantStats.DroppedNoLayoutNoSubscriber;

        // Rate-limited rather than per delta, for the same reason the subscriber path's drops are: a sender
        // keeps re-sending the entity's state, so the cause produces a line per delta, and at crowd scale a
        // line each is its own denial of service. The cause is an authoring fact about the class, so one
        // line says everything a line per delta would.
        UE_CLOG(ShouldReportStateSubscriberDrop(), LogCrowdyReplication, Warning,
            TEXT("[CrowdyEventRouter] State delta dropped — no rep layout for entity '%s' (ClassID=%lld); class has no CrowdyState properties or is not loaded, and no subscriber holds it. Further drops on this path are reported at most once a second."),
            *TargetParticipant->GetName(), Delta.ClassID);
        return;
    }

    // Coarse class guard before touching the blob: the sender's class id must match the resolved entity's. A
    // mismatch means sender and receiver disagree on the entity's class (drift/forgery); the LayoutHash guard
    // in Decode is the fine positional guard, this is the cheap early-out. (Both travel per FCrowdyStateDelta.)
    if (static_cast<FCrowdyClassID>(Delta.ClassID) != LocalClassID)
    {
        UE_LOG(LogCrowdyReplication, Warning,
            TEXT("[CrowdyEventRouter] State delta dropped — ClassID mismatch for entity '%s' (delta=%lld local=%u)."),
            *TargetParticipant->GetName(), Delta.ClassID, LocalClassID);
        return;
    }

    UObject* Container = ResolveStateContainer(TargetParticipant, Layout->OwnerClass.Get());
    if (!Container)
    {
        UE_LOG(LogCrowdyReplication, Warning,
            TEXT("[CrowdyEventRouter] State delta dropped — entity '%s' carries no '%s' to apply to."),
            *TargetParticipant->GetName(), *GetNameSafe(Layout->OwnerClass.Get()));
        return;
    }

    // The changed set is read twice below, once to fire the notifies and again to adopt a host correction, and
    // a notify can dispatch another delta on this router in between. The router's own array is therefore
    // claimed by the outermost dispatch only; a re-entrant one takes this local so it cannot write into the
    // set its caller is still using.
    TArray<int32> ReentrantChangedIndices;
    const bool bReentrantDispatch = bDispatchingParticipantStateDelta;
    TGuardValue<bool> DispatchGuard(bDispatchingParticipantStateDelta, true);
    TArray<int32>& ChangedIndices = bReentrantDispatch ? ReentrantChangedIndices : StateParticipantChangedIndices;

    // The codec writes each present value into Container via ContainerPtrToValuePtr, guarding the LayoutHash
    // (drop on mismatch, target untouched) and every untrusted read. ChangedIndices lists the applied slots.
    if (!FCrowdyStateCodec::Decode(*Layout, Delta.LayoutHash, Delta.Blob, Container, ChangedIndices,
        /*OutPresentIndices=*/nullptr, ResolveDecodeScratch(*Layout)))
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
    // Owned here rather than on the event, which only ever borrows: this local outlives the dispatch below.
    const FInstancedStruct Payload = FInstancedStruct::Make(Call);

    FCrowdyInboundEvent Event;
    Event.Payload  = &Payload;
    Event.bTargetedDelivery = true;
    Event.Target   = ECrowdyTarget::Everyone;
    Event.SenderID = FGuid();
    Event.TargetID = FGuid();
    Event.ReceivedAtSeconds = FPlatformTime::Seconds();

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
    // Owned here rather than on the event, which only ever borrows: this local outlives the dispatch below.
    const FInstancedStruct Payload = FInstancedStruct::Make(Delta);

    FCrowdyInboundEvent Event;
    Event.Payload = &Payload;
    Event.bTargetedDelivery = true;
    Event.Target = ECrowdyTarget::Everyone;
    Event.SenderID = FGuid();
    Event.TargetID = FGuid();
    Event.ReceivedAtSeconds = FPlatformTime::Seconds();

    DispatchStateDelta(Event, CrowdyStateMaxSpawnWaitAttempts);
}

void UCrowdyEventRouter::ReceiveChannelRpcCall(const FCrowdyRpcCall& Call)
{
    // A channel delivery is a multicast equivalent not a targeted single-actor send so the
    // broadcast echo-drop (by SenderID) and the owner/host-only guard both apply, exactly as for a
    // spatial Multicast. The originating client's id rides the payload, so the channel's wire UUID
    // is not needed here.
    // Owned here rather than on the event, which only ever borrows: this local outlives the dispatch below.
    const FInstancedStruct Payload = FInstancedStruct::Make(Call);

    FCrowdyInboundEvent Event;
    Event.Payload  = &Payload;
    Event.bTargetedDelivery = false;
    Event.SenderID = Call.SenderID;
    Event.Target   = ECrowdyTarget::Everyone;
    Event.TargetID = FGuid();
    Event.ReceivedAtSeconds = FPlatformTime::Seconds();

    DispatchRpcCall(Event, CrowdyRpcMaxSpawnWaitAttempts);
}

void UCrowdyEventRouter::ReceiveChannelStateDelta(const FCrowdyStateDelta& Delta)
{
    // A channel state delta is a multicast equivalent, not a targeted single-actor send, so the broadcast
    // echo-drop (by SenderID) and the receive-side authority gates all apply exactly as for a spatial
    // Multicast. The originating client's id rides the payload, so the channel's wire UUID is not needed.
    // Owned here rather than on the event, which only ever borrows: this local outlives the dispatch below.
    const FInstancedStruct Payload = FInstancedStruct::Make(Delta);

    FCrowdyInboundEvent Event;
    Event.Payload  = &Payload;
    Event.bTargetedDelivery = false;
    Event.SenderID = Delta.SenderID;
    Event.Target   = ECrowdyTarget::Everyone;
    Event.TargetID = FGuid();
    Event.ReceivedAtSeconds = FPlatformTime::Seconds();

    DispatchStateDelta(Event, CrowdyStateMaxSpawnWaitAttempts);
}

void UCrowdyEventRouter::DeferEvent(const FCrowdyInboundEvent& Event, int32 RemainingAttempts, double DeferBudgetSeconds)
{
    if (!Event.Payload)
    {
        return;
    }

    const FGuid EntityID = ResolveDeferredEntityID(*Event.Payload);

    if (RemainingAttempts <= 1)
    {
        UE_LOG(LogCrowdyReplication, Warning,
            TEXT("[CrowdyEventRouter] Deferred event for entity %s dropped — entity never spawned within the wait budget."),
            *EntityID.ToString());
        return;
    }

    // Both caps are applied before the event goes in, so the queue is never over either one, not even
    // between two statements. Every inbound event names an entity chosen by whoever sent it, so nothing
    // here may be allowed to grow with what arrives.
    EnforceDeferredEntityCap(EntityID);
    EnforceDeferredEventCap();

    FCrowdyDeferredEvent& Added = DeferredEvents.AddDefaulted_GetRef();

    // The one copy the receive path still makes, and the reason waiting is safe: the delivery that carried
    // this event borrows its bytes from the transport, so anything still queued when it returns has to own
    // what it holds. Left borrowing on the entry itself, which the queue relocates; a retry repoints it.
    Added.OwnedPayload = *Event.Payload;
    Added.Event = Event;
    Added.Event.Payload = nullptr;
    Added.RemainingAttempts = RemainingAttempts - 1;

    // Stamp the arrival for an event that came in by a path that does not stamp one. A re-defer carries
    // the stamp back in with the event, so the deadline below re-derives to the same instant however many
    // times the event goes round; deriving it from "now" instead would push it further out on every
    // retry and the deadline would never arrive.
    if (Added.Event.ReceivedAtSeconds <= 0.0)
    {
        Added.Event.ReceivedAtSeconds = FPlatformTime::Seconds();
    }
    Added.ExpiresAtSeconds = DeferBudgetSeconds > 0.0
        ? Added.Event.ReceivedAtSeconds + DeferBudgetSeconds
        : 0.0;
}

void UCrowdyEventRouter::EnforceDeferredEventCap()
{
    if (DeferredEvents.Num() < CrowdyDeferQueue::MaxEvents)
    {
        return;
    }

    // Oldest first: an event still waiting is one whose entity has not appeared, and the longer that has
    // been true the less likely it ever becomes true.
    const int32 NumToEvict = DeferredEvents.Num() - CrowdyDeferQueue::MaxEvents + 1;
    const FGuid OldestEntityID = ResolveDeferredEntityID(DeferredEvents[0].OwnedPayload);
    DeferredEvents.RemoveAt(0, NumToEvict, EAllowShrinking::No);
    ReportDeferredEviction(TEXT("the wait queue is full"), OldestEntityID, NumToEvict);
}

void UCrowdyEventRouter::EnforceDeferredEntityCap(const FGuid& EntityID)
{
    // Fewer events than the cap cannot be more entities than the cap, whatever they are waiting on, so
    // the ordinary case never walks the queue at all.
    if (DeferredEvents.Num() < CrowdyDeferQueue::MaxDistinctEntities)
    {
        return;
    }

    // Rebuilt from the queue each time rather than kept alongside it. The queue is the only thing that
    // knows what is waiting, and a second structure tracking the same facts is one that can disagree
    // with it after an eviction, a retry or a re-defer. That costs a walk of the queue on each defer once
    // it is this full, which is the trade: correct under every mutation, at a bounded cost.
    TSet<FGuid> Waiting;
    TArray<FGuid> WaitingOrder;
    Waiting.Reserve(DeferredEvents.Num());
    WaitingOrder.Reserve(DeferredEvents.Num());
    for (const FCrowdyDeferredEvent& Deferred : DeferredEvents)
    {
        bool bAlreadyWaiting = false;
        const FGuid Held = ResolveDeferredEntityID(Deferred.OwnedPayload);
        Waiting.Add(Held, &bAlreadyWaiting);
        if (!bAlreadyWaiting)
        {
            WaitingOrder.Add(Held);
        }
    }

    // Another event for an entity already waited on costs no new id, so the cap is not involved.
    if (Waiting.Contains(EntityID))
    {
        return;
    }

    // Evict whole entities, oldest first, until there is room for one more. Per-entity rather than
    // per-event because the attack this bounds is one event each for a great many invented ids, which
    // stays under any per-id limit while filling the queue and pushing out every legitimate wait.
    int32 Index = 0;
    while (WaitingOrder.Num() - Index >= CrowdyDeferQueue::MaxDistinctEntities)
    {
        const FGuid Evicted = WaitingOrder[Index++];
        const int32 NumEvicted = DeferredEvents.RemoveAll(
            [&Evicted](const FCrowdyDeferredEvent& Deferred)
            {
                return ResolveDeferredEntityID(Deferred.OwnedPayload) == Evicted;
            });
        ReportDeferredEviction(TEXT("too many distinct entities are being waited on"), Evicted, NumEvicted);
    }
}

void UCrowdyEventRouter::ReportDeferredEviction(const TCHAR* Reason, const FGuid& EntityID, int32 NumEvicted)
{
    if (NumEvicted <= 0)
    {
        return;
    }

    DeferredEvictionCount += NumEvicted;
    DeferredEvictionsSinceLog += NumEvicted;

    // Every eviction is counted, and the count is what gets reported: a queue that quietly shrinks looks
    // exactly like a queue that is working. The line is rate-gated because eviction under a flood happens
    // once per arriving event, and one line each would let the flood drive the log as well as the queue.
    const double Now = FPlatformTime::Seconds();
    if (LastEvictionLogSeconds > 0.0 && Now - LastEvictionLogSeconds < 1.0)
    {
        return;
    }

    UE_LOG(LogCrowdyReplication, Warning,
        TEXT("[CrowdyEventRouter] Dropped %d deferred event(s) since the last report - %s; the most recent was waiting on entity %s. %d dropped in total."),
        DeferredEvictionsSinceLog, Reason, *EntityID.ToString(), DeferredEvictionCount);

    DeferredEvictionsSinceLog = 0;
    LastEvictionLogSeconds = Now;
}

int32 UCrowdyEventRouter::NumDeferredEntitiesForTest() const
{
    TSet<FGuid> Waiting;
    Waiting.Reserve(DeferredEvents.Num());
    for (const FCrowdyDeferredEvent& Deferred : DeferredEvents)
    {
        Waiting.Add(ResolveDeferredEntityID(Deferred.OwnedPayload));
    }
    return Waiting.Num();
}

void UCrowdyEventRouter::RetryDeferredEvents()
{
    if (DeferredEvents.IsEmpty())
        return;

    // Swap the pending set out first so events that still can't resolve re-defer into a
    // fresh list for the next tick instead of looping within this one.
    TArray<FCrowdyDeferredEvent> Pending = MoveTemp(DeferredEvents);
    DeferredEvents.Reset();

    const double Now = FPlatformTime::Seconds();

    for (const FCrowdyDeferredEvent& Deferred : Pending)
    {
        // An event carrying a deadline is judged on how long it has existed, not on how many times it has
        // been retried: a retry count means whatever the frame rate happens to be, so the same budget is
        // seconds on one machine and a fraction of one on another.
        if (Deferred.ExpiresAtSeconds > 0.0 && Now > Deferred.ExpiresAtSeconds)
        {
            ReportDeferredEviction(TEXT("it passed its deadline"), ResolveDeferredEntityID(Deferred.OwnedPayload), 1);
            continue;
        }

        // The view is built here rather than kept on the queued entry, because Pending's elements are the
        // only stable addresses this payload has ever had: the queue it came off relocates on every
        // eviction and every growth.
        FCrowdyInboundEvent Retry = Deferred.Event;
        Retry.Payload = &Deferred.OwnedPayload;

        const UScriptStruct* PayloadType = Deferred.OwnedPayload.GetScriptStruct();
        if (PayloadType == FCrowdyRpcCall::StaticStruct())
        {
            DispatchRpcCall(Retry, Deferred.RemainingAttempts);
        }
        else if (PayloadType == FCrowdyStateDelta::StaticStruct())
        {
            DispatchStateDelta(Retry, Deferred.RemainingAttempts);
        }
        // Only these two payload types are ever deferred, so no else branch is needed.
    }
}

void UCrowdyEventRouter::Tick(float DeltaTime)
{
    // Events are routed the moment they arrive; the only per-frame work left is the retry ladder for
    // events that reached this client before the entity they address had spawned.
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

void UCrowdyEventRouter::HandleDelivery(const FCrowdyDelivery& Delivery)
{
    // A model-driven SERVER_EVENT (139) carries no decoded FInstancedStruct at all (its state is raw
    // application bytes, not a serialized CrowdyEvent), so there is nothing here to dispatch. Every other
    // event that reaches this fallback arrives with a struct that resolved fine but that nothing else
    // claimed, and dispatching those is exactly this fallback's job.
    if (!Delivery.Payload)
        return;

    const bool bTargeted = Delivery.Opcode == ECrowdyMessageType::SINGLE_ACTOR_MESSAGE;

    // A single-actor message decodes into an FSingleActorNotification, which derives FGameEventNotification,
    // so the cast holds for either opcode reaching here.
    const auto& EventMessage = static_cast<const FGameEventNotification&>(Delivery.Get());

    UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[CrowdyEventRouter] Received event %s"), *EventMessage.GetTypeName().ToString());
    // Events claimed by another subscriber (entity lifecycle, the Game Model, …) are Handled and never
    // reach this fallback; it only sees event types nothing else claimed.

    FCrowdyInboundEvent InboundEvent;
    // Borrowed from the message, which outlives this dispatch. Only the wait queue copies it, and only for
    // the events it actually holds.
    InboundEvent.Payload  = Delivery.Payload;
    InboundEvent.bTargetedDelivery = bTargeted;
    // Stamped once, here at the door. Everything downstream that asks how old this event is reads this
    // value, so it must not be re-stamped later: an event that waits and retries would otherwise look
    // newly arrived on every pass.
    InboundEvent.ReceivedAtSeconds = FPlatformTime::Seconds();
    // A single-actor message's header UUID is the destination, not the sender, so the sender
    // (when a receiver needs it) rides the payload; only a broadcast's UUID identifies the sender.
    InboundEvent.SenderID = bTargeted ? FGuid() : USerializationFunctionLibrary::ToGuid(EventMessage.UUID);
    InboundEvent.Target   = EventMessage.Target;
    InboundEvent.TargetID = EventMessage.TargetID;

    UE_CLOG(CrowdyReplicationTrace::Entity(), LogCrowdyReplication, Log, TEXT("[CrowdyEventRouter]: Event Payload %s, SenderID: %s, TargetID: %s, Target: %d"),
        *InboundEvent.Payload->GetScriptStruct()->GetName(),
        *InboundEvent.SenderID.ToString(),
        *InboundEvent.TargetID.ToString(),
        InboundEvent.Target);

    DispatchEvent(InboundEvent);
}

bool UCrowdyEventRouter::LoadConfig() const
{
    const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(GetWorld());
    return Profile && Profile->bEnableNetworking;
}
