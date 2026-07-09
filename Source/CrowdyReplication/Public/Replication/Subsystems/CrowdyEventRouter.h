// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Structures/FCrowdyEventContext.h"
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/UObjectArray.h"
#include "CrowdyEventRouter.generated.h"


class UCrowdySDKBridgeSubsystem;
class UCrowdyEntitySubsystem;
class UCrowdyAutoRegistry;
class UCrowdyStateReplicator;
struct FCrowdyRpcCall;
struct FCrowdyStateDelta;

/**
 * A game event as received off the wire, with its envelope.
 */
struct FCrowdyInboundEvent
{
	FInstancedStruct Payload;
	FGuid SenderID;
	ECrowdyTarget Target = ECrowdyTarget::Everyone;
	FGuid TargetID;

	// True when this arrived as a SINGLE_ACTOR_MESSAGE (the server already delivered it only to
	// the target's owner), false for a spatial broadcast. The RPC receive path uses this to skip
	// the broadcast-only echo-drop and authority check.
	bool bTargetedDelivery = false;
};

/**
 * An inbound event (an RPC call or a state delta) whose target entity has not registered yet, held for
 * a bounded number of retries so an event that races ahead of its entity's spawn event still applies
 * once the entity registers.
 */
struct FCrowdyDeferredEvent
{
	FCrowdyInboundEvent Event;
	int32 RemainingAttempts = 0;
};

/**
 * Receives game events off the wire and routes them to registered handler
 * objects according to the event envelope (see ECrowdyTarget).
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyEventRouter : public UTickableWorldSubsystem, public ICrowdyReceptionLayer
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Routes the event to handlers according to its envelope (Target/TargetID)
	void DispatchEvent(const FCrowdyInboundEvent& Event);

	// Debug loopback (crowdy.rpc.loopback): runs a call this client just sent through its own
	// receive path as a plain Everyone-addressed announcement, so the serialize/resolve/dispatch
	// round-trip can be exercised with a single client. FCrowdyRPC guards against re-entry.
	void ReceiveLoopbackCall(const FCrowdyRpcCall& Call);

	// Debug loopback (crowdy.state.loopback): replays a CrowdyState delta the UCrowdyStateReplicator just
	// sent onto a distinct local "mirror" entity (a second instance of the same class, RemoteProxy role,
	// a different NetID), so a single PIE client can exercise decode + OnRep + the receive-side gates
	// without a second client. Unlike ReceiveLoopbackCall this cannot replay onto the sending entity
	// itself: DispatchStateDelta's ownership gate drops a non-host-sourced delta for an entity we own, and
	// decoding onto the same live actor that produced the delta would report zero changed properties. The
	// caller (UCrowdyStateReplicator) is responsible for cloning the outgoing delta, overwriting EntityID
	// with the mirror's NetID, and clearing SenderID before calling this; DispatchStateDelta itself is
	// unmodified.
	void ReceiveLoopbackStateDelta(const FCrowdyStateDelta& Delta);

	// Receive entry for a reliable RPC delivered over the session channel. UCrowdyChannels decodes
	// the channel payload and calls this; the call is queued and dispatched like a spatial Multicast
	// (the sender's own echo is dropped by SenderID, the body runs once on every other member). Safe
	// to call from any thread.
	void ReceiveChannelRpcCall(const FCrowdyRpcCall& Call);

	// Receive entry for a CrowdyState delta delivered over the session channel (the non-spatial transport for
	// subsystem participants). UCrowdyChannels discriminates the channel payload by its leading kind tag,
	// decodes it via DecodeChannelStateDelta, and calls this; the delta is queued and dispatched like a
	// spatial Multicast through the unchanged DispatchStateDelta (self-echo dropped by SenderID, applied on
	// every other member). Safe to call from any thread.
	void ReceiveChannelStateDelta(const FCrowdyStateDelta& Delta);

	// UTickableWorldSubsystem
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdyEventDispatcher, STATGROUP_Tickables);
	}

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	// Reception Layer API
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) override;
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const override;
	virtual TArray<UScriptStruct*> GetSupportedEvents() const override;

	// Resolves the object a state delta applies to: the actor itself when it IsA the layout's owner
	// class, else its first component that IsA it (deterministic GetComponents() order), else null.
	// Mirrors the file-static ResolveRpcReceiver but keys on the layout's UClass; public+static so the
	// container-resolution cases are directly unit-testable without a world.
	static UObject* ResolveStateContainer(AActor* Actor, const UClass* LayoutOwnerClass);

	// UObject participant overload: the participant itself when it IsA the layout's owner class, else (when it is
	// an actor) its first component that IsA it, else null. The AActor* overload forwards here.
	static UObject* ResolveStateContainer(UObject* Participant, const UClass* LayoutOwnerClass);

	// Test seams (headless, no world/bridge): inject the collaborators Initialize would resolve and drive
	// the deferred-retry pass. DispatchEvent is already public, so a test feeds an inbound event straight
	// through it.
	void SetEntitySubsystemForTest(UCrowdyEntitySubsystem* In) { EntitySubsystem = In; }
	void SetAutoRegistryForTest(UCrowdyAutoRegistry* In) { AutoRegistry = In; }
	void RetryDeferredForTest() { RetryDeferredEvents(); }
	int32 NumDeferredForTest() const { return DeferredEvents.Num(); }

	// Injects the replicator the apply path adopts host corrections into (see AdoptHostValues). In production
	// the router resolves it from the world; a headless apply test has no world, so it injects one directly.
	void SetStateReplicatorForTest(UCrowdyStateReplicator* In) { StateReplicatorForTest = In; }

private:

	UCrowdySDKBridgeSubsystem* Bridge = nullptr;
	UCrowdyEntitySubsystem* EntitySubsystem = nullptr;
	UCrowdyAutoRegistry* AutoRegistry = nullptr;
	UCrowdyStateReplicator* StateReplicatorForTest = nullptr;

	// Queue for events
	TQueue<FCrowdyInboundEvent, EQueueMode::Mpsc> EventQueue;

	// Inbound events awaiting their target entity's spawn (see FCrowdyDeferredEvent)
	TArray<FCrowdyDeferredEvent> DeferredEvents;

	// RPC-style CrowdyEvent receive path (FCrowdyRpcCall payloads): resolve the target
	// entity and UFunction, apply the client-authoritative ownership model, and invoke
	// the implementation. See [[rpc-event-system-design]] ownership model J.
	void DispatchRpcCall(const FCrowdyInboundEvent& Event, int32 RemainingAttempts);

	// CrowdyState receive path (FCrowdyStateDelta payloads): resolve the target entity and apply
	// container, decode the changed values onto the live container via FCrowdyStateCodec, and fire each
	// changed property's parameterless CrowdyOnRep notify. For an entity WE own, applies host-precedence by
	// convention: a foreign non-host delta is dropped before decode, and a HostSourced correction is applied
	// and then adopted into the owner's shadow (AdoptHostValues) so the owner does not revert it. Defers to
	// the shared spawn-wait retry when the entity has not registered yet.
	void DispatchStateDelta(const FCrowdyInboundEvent& Event, int32 RemainingAttempts);

	// Resolves the replicator the apply path adopts host corrections into: the injected one in tests, else the
	// world's UCrowdyStateReplicator. Null when there is no world (headless) and none was injected.
	UCrowdyStateReplicator* ResolveStateReplicator() const;

	void DeferEvent(const FCrowdyInboundEvent& Event, int32 RemainingAttempts);
	void RetryDeferredEvents();

	bool LoadConfig() const;
};
