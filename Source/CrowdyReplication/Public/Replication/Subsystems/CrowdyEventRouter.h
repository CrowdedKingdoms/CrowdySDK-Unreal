// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
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
struct FCrowdyRpcCall;

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
 * An inbound RPC whose target entity has not spawned yet, held for a bounded number of
 * retries so a call that races ahead of its entity's spawn event still fires once the
 * entity registers.
 */
struct FCrowdyDeferredRpcCall
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

	// Receive entry for a reliable RPC delivered over the session channel. UCrowdyChannels decodes
	// the channel payload and calls this; the call is queued and dispatched like a spatial Multicast
	// (the sender's own echo is dropped by SenderID, the body runs once on every other member). Safe
	// to call from any thread.
	void ReceiveChannelRpcCall(const FCrowdyRpcCall& Call);

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

private:

	UCrowdySDKBridgeSubsystem* Bridge = nullptr;
	UCrowdyEntitySubsystem* EntitySubsystem = nullptr;
	UCrowdyAutoRegistry* AutoRegistry = nullptr;

	// Queue for events
	TQueue<FCrowdyInboundEvent, EQueueMode::Mpsc> EventQueue;

	// Inbound RPCs awaiting their target entity's spawn (see FCrowdyDeferredRpcCall)
	TArray<FCrowdyDeferredRpcCall> DeferredRpcCalls;

	// RPC-style CrowdyEvent receive path (FCrowdyRpcCall payloads): resolve the target
	// entity and UFunction, apply the client-authoritative ownership model, and invoke
	// the implementation. See [[rpc-event-system-design]] ownership model J.
	void DispatchRpcCall(const FCrowdyInboundEvent& Event, int32 RemainingAttempts);
	void DeferRpcCall(const FCrowdyInboundEvent& Event, int32 RemainingAttempts);
	void RetryDeferredRpcCalls();

	bool LoadConfig() const;
};
