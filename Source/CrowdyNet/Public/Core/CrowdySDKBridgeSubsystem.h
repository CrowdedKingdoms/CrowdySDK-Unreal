#pragma once
#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyMessage.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryRequest.h"
#include "Internal/FCrowdyDataRegistry.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/Function.h"
#include "CrowdySDKBridgeSubsystem.generated.h"

class FCrowdyServiceRegistry;

/**
 * Thin mediator set up by UCrowdySDKSubsystem so that CrowdyReplication
 * subsystems can register reception layers and dispatch messages without
 * holding a direct pointer back to UCrowdySDKSubsystem (which would be circular).
 */
UCLASS()
class CROWDYNET_API UCrowdySDKBridgeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	FCrowdyServiceRegistry* ServiceRegistry = nullptr;
	FCrowdyDataRegistry*    DataRegistry    = nullptr;

	TFunction<void(int64, int64, int64,
		ECrowdyDecayRate, ECrowdyReplicationDistance,
		const FString&, const FInstancedStruct&, bool)> DispatchActorUpdateFn;

	TFunction<void(int64, int64, int64,
		ECrowdyDecayRate, ECrowdyReplicationDistance,
		const FGuid&, FInstancedStruct,
		ECrowdyTarget, const FGuid&, bool)> DispatchGameEventFn;

	// Actor-to-actor send (SINGLE_ACTOR_MESSAGE). Args: target chunk X/Y/Z, the destination
	// actor's UUID (the server delivers only to its owner), the payload, and bAsync.
	TFunction<void(int64, int64, int64,
		const FGuid&, FInstancedStruct, bool)> DispatchSingleActorMessageFn;

	// Publishes an already-encoded reliable RPC payload over a named channel (CHANNEL_MESSAGE_REQUEST);
	// an empty name means the app-wide default session channel. Set by UCrowdyChannels; lets the RPC
	// send path in CrowdyReplication reach the channel transport in CrowdyServices without depending on it.
	TFunction<void(const FString&, const TArray<uint8>&)> PublishReliableRpcFn;

	TFunction<void(const ICrowdyMessage&)>  SendMessageFn;
	TFunction<void(ICrowdyQueryRequest&)>  ExecuteQueryFn;
	TFunction<void()>                      BroadcastHUDReadyFn;

	// Re-reads UCrowdySDKDeveloperSettings into the live session (endpoints, app id, UDP protocol)
	// so an editor Config Sync can take effect without relaunching Play. Set by UCrowdySDKSubsystem.
	TFunction<void()>                      ReloadConfigFn;
};
