#pragma once
#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/Function.h"
#include "CrowdySDKBridgeSubsystem.generated.h"

class FCrowdyServiceRegistry;

/**
 * Thin mediator set up by UCrowdySDKSubsystem so that CrowdyReplication
 * subsystems can reach the service registry to subscribe, and dispatch messages,
 * without holding a direct pointer back to UCrowdySDKSubsystem (which would be circular).
 */
UCLASS()
class CROWDYNET_API UCrowdySDKBridgeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	FCrowdyServiceRegistry* ServiceRegistry = nullptr;

	// The FCrowdyClassID names the entity's class and rides every update, because the transport is lossy
	// and relevance gated: a receiver can start listening at any instant, so a class announced once is
	// lost to everyone who was not there for it.
	TFunction<void(int64, int64, int64,
		ECrowdyDecayRate, ECrowdyReplicationDistance,
		const FString&, const FInstancedStruct&, FCrowdyClassID, bool)> DispatchActorUpdateFn;

	// Says an actor is still where it was without restating its state (CLIENT_ACTOR_HEARTBEAT). Args: the actor's
	// chunk X/Y/Z and its UUID. Carries no state and no payload, so there is nothing to serialize and nothing to
	// gain from deferring it off the calling thread.
	TFunction<void(int64, int64, int64, const FString&)> DispatchActorHeartbeatFn;

	TFunction<void(int64, int64, int64,
		ECrowdyDecayRate, ECrowdyReplicationDistance,
		const FGuid&, FInstancedStruct,
		ECrowdyTarget, const FGuid&, bool)> DispatchGameEventFn;

	// The same send taking a BORROWED view of the payload (its struct and its memory) instead of owning an
	// FInstancedStruct, so a caller that already holds the value does not build and copy one to send it.
	// Deliberately carries no bAsync: the view is only valid for the duration of the call, so this send is
	// synchronous by construction and cannot be deferred onto a task that would outlive the caller's value.
	TFunction<void(int64, int64, int64,
		ECrowdyDecayRate, ECrowdyReplicationDistance,
		const FGuid&, const UScriptStruct*, const void*,
		ECrowdyTarget, const FGuid&)> DispatchGameEventViewFn;

	// Actor-to-actor send (SINGLE_ACTOR_MESSAGE). Args: target chunk X/Y/Z, the destination
	// actor's UUID (the server delivers only to its owner), the payload, and bAsync.
	TFunction<void(int64, int64, int64,
		const FGuid&, FInstancedStruct, bool)> DispatchSingleActorMessageFn;

	// Publishes an already-encoded reliable RPC payload over a named channel (CHANNEL_MESSAGE_REQUEST);
	// an empty name means the app-wide default session channel. Set by UCrowdyChannels; lets the RPC
	// send path in CrowdyReplication reach the channel transport in CrowdyServices without depending on it.
	TFunction<void(const FString&, const TArray<uint8>&)> PublishReliableRpcFn;

	TFunction<void(const ICrowdyMessage&)>  SendMessageFn;
	TFunction<void()>                      BroadcastHUDReadyFn;

	// Re-reads UCrowdySDKDeveloperSettings into the live session (endpoints, app id, UDP protocol)
	// so an editor Config Sync can take effect without relaunching Play. Set by UCrowdySDKSubsystem.
	TFunction<void()>                      ReloadConfigFn;
};
