// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "CrowdyCppClient.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Shared/Types/Structures/Versioning/FGameVersion.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"  
#include "StructUtils/InstancedStruct.h"// EUDPConnectionState
#include "Subsystem/CrowdyAuthentication.h"
#include "CrowdySDKSubsystem.generated.h"


class UCrowdyPersistenceSubsystem;

// Structs
struct FCrowdyUDPEndpoint;
struct FGameSessionInfo;

// Interfaces
class ICrowdyMessage;

// Core Classes
class FCrowdyServiceRegistry;
class FCrowdyMessageParser;
class FVoiceChatService;

// Unreal Subsystems
class UCrowdyWorkerThreadsSubsystem;
class UCrowdyGameSession;
class UVoiceChatSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnLogin, bool, bSuccess, FString, GameToken);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnRegister, bool, bSuccess, FString, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnLogout, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnUDPAddressNotify, bool, bSuccess, bool, GateKeep);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnVersionInfo, FGameVersion, ServerVersion, FGameVersion, ClientVersion);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnUDPConnectionSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnUDPTimedOut);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTeleportPermission, bool, bAllowed);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnCrowdyHUDReady);

/**
 *
 */
UCLASS(BlueprintType, meta = (DisplayName = "Crowdy SDK Subsystem"))
class CROWDYSDK_API UCrowdySDKSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
	
public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	UPROPERTY(BlueprintAssignable, Category = "CrowdySDK|Authentication")
	FOnLogin OnLogin;
	
	UPROPERTY(BlueprintAssignable, Category = "CrowdySDK|Authentication")
	FOnRegister OnRegister;
	
	UPROPERTY(BlueprintAssignable, Category = "CrowdySDK|Authentication")
	FOnLogout OnLogout;
	
	UPROPERTY(BlueprintAssignable, Category = "CrowdySDK|Authentication", meta=(DisplayName="On UDP Address Notify"))
	FOnUDPAddressNotify OnUDPAddressNotify;
	
	UPROPERTY(BlueprintAssignable, Category = "CrowdySDK|Authentication")
	FOnVersionInfo OnVersionInfo;
	
	UPROPERTY(BlueprintAssignable, Category = "CrowdySDK|Connection", meta=(DisplayName="On UDP Connection Success"))
	FOnUDPConnectionSuccess OnUDPConnectionSuccess;
	
	UPROPERTY(BlueprintAssignable, Category = "CrowdySDK|Connection", meta=(DisplayName="On UDP Timed Out"))
	FOnUDPTimedOut OnUDPTimedOut;
	
	UPROPERTY(BlueprintAssignable, Category= "CrowdySDK|Permissions")
	FOnTeleportPermission OnTeleportPermission;
	
	UPROPERTY(BlueprintAssignable, Category = "CrowdySDK|HUD", meta=(DisplayName="Crowdy HUD Ready"))
	FOnCrowdyHUDReady OnCrowdyHUDReady;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void Login(const FString Email, const FString Password) const;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void Register(const FString Email, const FString Password) const;

	/** Magic-link step 2 complete sign-in with the one-time token. Result on OnLogin.
	 *  Pair with UCrowdyAuthentication::RequestLoginLink (step 1). */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void CompleteLoginLink(const FString Token) const;

	/** One-call magic-link sign-in: opens a loopback listener, requests the email link, and
	 *  completes automatically when the user clicks it. Result on OnLogin. */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void BeginMagicLinkSignIn(const FString Email) const;

	/** One-call social (OAuth) sign-in via a loopback listener + system browser. Provider comes
	 *  from UCrowdyAuthentication::GetAvailableLoginProviders (e.g. "google"). Result on OnLogin. */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void BeginSocialSignIn(const FString Provider) const;

	/** Manually rotate the app-scoped token (also happens automatically before expiry). */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void RefreshAppToken() const;

	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void Logout() const;

	// Kept for existing call sites; it no longer has to do anything. The shared API client re-reads both
	// endpoints from UCrowdySDKDeveloperSettings every time it is resolved and rebuilds itself when they
	// change, so a CrowdyStudio Config Sync already reaches a running PIE session on its own.
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Network")
	void ReloadEndpointsFromSettings();

	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void SetGameSessionInfo(const FGameSessionInfo GameSessionInfo);
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication", meta=(DisplayName="Request UDP Access"))
	void RequestUDPAccess() const;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void RequestVersionInfo() const;
	
	/**
	 * @deprecated Use SetGameApiUrl for the gameplay endpoint, or SetDiscoveryUrl for the shared origin.
	 *
	 * One setter cannot serve both: they are not two planes of one API but a specific datacenter's instance and
	 * the name every datacenter answers to.
	 */
	UE_DEPRECATED(5.8, "Use SetGameApiUrl for the gameplay endpoint, or SetDiscoveryUrl for the shared origin.")
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication",
		meta=(DisplayName="Set Query Endpoint (Deprecated)",
		      DeprecatedFunction,
		      DeprecationMessage="Use Set Game Api Url for the gameplay endpoint, or Set Discovery Url for the shared origin."))
	void SetQueryEndpoint(const FString InEndpoint) const;

	/** Stores the shared origin for this session: the name every datacenter answers, used to resolve where an app
	 *  lives and to sign in. Only the Custom backend reads it; Dev and Production resolve to their own built-in
	 *  hosts, and the call warns when it cannot take effect. */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Configuration")
	void SetDiscoveryUrl(const FString InUrl) const;

	/** Stores the Game API HTTP endpoint for this session. This names one datacenter's instance, so it is normally
	 *  resolved from the shared origin rather than set by hand. */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Configuration")
	void SetGameApiUrl(const FString InHttpUrl) const;
	
	/**
	 * Returns the current UDP connection state. Poll this to drive connection
	 * indicators in your UI no delegates or event subscriptions required.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CrowdySDK|Connection",
		meta=(DisplayName="Get UDP Connection State"))
	EUDPConnectionState GetUDPConnectionState() const;

	/**
	 * @deprecated Remove the call. Nothing replaces it.
	 *
	 * The threshold passed here is ignored: the connection watches its own silence, using the interval set by
	 * 'UDP Timeout (seconds)' in Project Settings, Plugins, Crowdy SDK. The round-trip ping this used to start
	 * is now started automatically when the connection comes up, so calling this has no effect at all.
	 */
	UE_DEPRECATED(5.8, "Remove the call. The round-trip ping starts automatically when the connection comes up, and the connection tracks its own liveness.")
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection",
		meta=(DisplayName="Start UDP Timeout Monitoring (Deprecated)",
		      DeprecatedFunction,
		      DeprecationMessage="Remove this node. The round-trip ping now starts automatically when the connection comes up, and the connection tracks its own liveness using 'UDP Timeout (seconds)' in Project Settings."))
	void StartUDPTimeoutMonitoring(const float ThresholdSeconds = 30.0f);

	/**
	 * @deprecated Remove the call unless you specifically want to stop measuring latency.
	 *
	 * This stops the round-trip ping, which is all it ever did. It does not affect whether the connection is
	 * watched for silence, which is not something a caller starts or stops.
	 */
	UE_DEPRECATED(5.8, "Remove the call unless you specifically want to stop the latency ping; it has never affected connection liveness.")
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection",
		meta=(DisplayName="Stop UDP Timeout Monitoring (Deprecated)",
		      DeprecatedFunction,
		      DeprecationMessage="Remove this node unless you specifically want to stop the latency ping. It has never affected connection liveness."))
	void StopUDPTimeoutMonitoring();

	/**
	 * Reports the session as disconnected, which is what connection UI reads. It does not close the connection or
	 * stop traffic: the connection is owned by the transport and recovers on its own, and interrupting a recovery
	 * already under way is not something a caller should be able to do by asking the UI to show a disconnect.
	 */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection")
	void StopNetworkOperations() const;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection")
	void ToggleNetworkMessageProcessing() const;
	
	/**
	 * @deprecated Remove the call. Nothing replaces it.
	 *
	 * Does nothing. Connection liveness used to be inferred from traffic arriving here, so a system that received
	 * a message by another route could refresh the clock by calling this. The connection now tracks its own
	 * liveness, so there is nothing left to refresh. Kept only so an existing graph that calls it keeps working
	 * unchanged.
	 */
	UE_DEPRECATED(5.8, "Remove the call. The connection tracks its own liveness, so this does nothing.")
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection",
		meta=(DisplayName="Trigger UDP Heartbeat (Deprecated)",
		      DeprecatedFunction,
		      DeprecationMessage="Remove this node. The connection tracks its own liveness, so this does nothing."))
	void TriggerUdpHeartbeat() const;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Communication")
	void StartVoiceChat();
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Communication")
	void StopVoiceChat();
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Communication")
	void PlayVoiceChat();
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Communication")
	void MuteVoiceChat();
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Communication")
	void SetVoiceChatStreamTimeoutThreshold(const float InSeconds);
	
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Communication")
	void ToggleOwnerEcho(bool bEnable) const;
	
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Permissions")
	void RequestTeleportPermission(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ, const int32 VoxelX,
	                               const int32 VoxelY, const int32 VoxelZ) const;
	
	/**
	 * Does nothing. Inbound messages used to be received through registered layers that had to be
	 * released together on teardown; a system now keeps a subscription handle for as long as it wants
	 * to receive messages, and delivery stops automatically when that handle is destroyed along with
	 * its owner. There is nothing left to release in bulk, so this call has no effect. Kept only so an
	 * existing Blueprint graph that calls it keeps working unchanged.
	 */
	UE_DEPRECATED(5.8, "Remove the call. A system keeps a subscription handle for as long as it wants messages, and delivery stops when that handle is destroyed with its owner.")
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Reception Layer",
		meta=(DisplayName="Deregister All Reception Layers (Deprecated)",
		      DeprecatedFunction,
		      DeprecationMessage="Remove this node. A system now keeps a subscription handle for as long as it wants to receive messages, and delivery stops automatically when that handle is destroyed along with its owner. There is nothing left to release in bulk, so this does nothing."))
	void DeregisterAllReceptionLayers();
	
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Subsystem")
	void SetExpectedActorUpdateStateSize(const int32 InSize) const;
	
	/**
	 * Puts one actor update on the wire. C++ only, where it used to be BlueprintCallable.
	 *
	 * ClassID is the entity class the update speaks for, and it rides every update because the transport
	 * is lossy and relevance gated: a receiver can begin listening at any instant, so a class announced
	 * once is lost to everyone who was not listening for it. It is a wire identity from
	 * UCrowdyClassRegistry rather than anything a graph can meaningfully produce, and its type is an
	 * alias for uint32, which is neither resolvable by UHT nor a Blueprint property type. Blueprints
	 * replicate an actor by putting a UCrowdyEntityComponent on it, which is the supported surface and
	 * is unaffected. Nothing in this project or its plugins called this node.
	 */
	void DispatchActorUpdate(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
	                         const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance,
	                         const FString& InstigatorID, const FInstancedStruct& ActorStatePayload,
	                         FCrowdyClassID ClassID, bool bAsync = false);

	/**
	 * Say an actor is still present without restating its state. Costs the spatial header alone, so an actor that
	 * has not changed can stay present for a fraction of what repeating its full state would cost.
	 */
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Replication|Actor Updates")
	void DispatchActorHeartbeat(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
	                            const FString& InstigatorID);

	/**
	 * Dispatch a game event. The EventPayload pin accepts any struct type directly.
	 * Target/TargetID address the event; Everyone broadcasts (legacy behavior).
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category="CrowdySDK|Replication|Events",
		meta=(DisplayName="Dispatch Game Event", CustomStructureParam="EventPayload", AutoCreateRefTerm="TargetID"))
	void K2_DispatchGameEvent(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
	                          const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance,
	                          const FGuid& InstigatorID, const int32& EventPayload,
	                          const ECrowdyTarget Target, const FGuid& TargetID, bool bAsync = false);
	DECLARE_FUNCTION(execK2_DispatchGameEvent);

	/**
	 * Dispatch a game event from C++. Pass any USTRUCT directly, no FInstancedStruct wrapper needed.
	 */
	template<typename T>
	void DispatchGameEvent(
		int64 ChunkX, int64 ChunkY, int64 ChunkZ,
		ECrowdyDecayRate DecayRate,
		ECrowdyReplicationDistance ReplicationDistance,
		const FGuid& InstigatorID,
		const T& EventPayload,
		ECrowdyTarget Target = ECrowdyTarget::Everyone,
		const FGuid& TargetID = FGuid(),
		bool bAsync = false)
	{
		DispatchGameEvent_Internal(ChunkX, ChunkY, ChunkZ, DecayRate, ReplicationDistance, InstigatorID,
			FInstancedStruct::Make<T>(EventPayload), Target, TargetID, bAsync);
	}

	/** Shared implementation called by the template overload, the custom thunk, and the deprecated wrapper. */
	void DispatchGameEvent_Internal(int64 ChunkX, int64 ChunkY, int64 ChunkZ,
	                                ECrowdyDecayRate DecayRate, ECrowdyReplicationDistance ReplicationDistance,
	                                const FGuid& InstigatorID, FInstancedStruct EventPayload,
	                                ECrowdyTarget Target, const FGuid& TargetID, bool bAsync);

	/**
	 * The whole of putting one game event on the wire, from a BORROWED view of the payload. Every other
	 * entry point ends here, including the FInstancedStruct one above, which owns the value and optionally
	 * defers before calling this; so there is a single send and a single description of the frame.
	 *
	 * The view is read during the call only, which is why this takes no bAsync: deferring it would outlive
	 * the value it points at. A caller that needs to defer owns the payload and uses the overload above.
	 */
	void DispatchGameEventView_Internal(int64 ChunkX, int64 ChunkY, int64 ChunkZ,
	                                    ECrowdyDecayRate DecayRate, ECrowdyReplicationDistance ReplicationDistance,
	                                    const FGuid& InstigatorID, const UScriptStruct* PayloadStruct,
	                                    const void* PayloadMemory, ECrowdyTarget Target, const FGuid& TargetID);

	/** Actor-to-actor send (SINGLE_ACTOR_MESSAGE): same payload as a game event, but the server
	 *  delivers it only to the client that owns TargetActorID. UUID = TargetActorID, chunk = the
	 *  target's chunk; distance/decay are unused. */
	void DispatchSingleActorMessage_Internal(int64 ChunkX, int64 ChunkY, int64 ChunkZ,
	                                          const FGuid& TargetActorID, FInstancedStruct EventPayload, bool bAsync);

	/**
	 * Subscribing to inbound messages from outside this plugin.
	 *
	 * These forward to the message router. Each returns a handle whose lifetime is the subscription's:
	 * keep it as a member of the object whose state the callback touches, and let it be destroyed with
	 * that object. Releasing it, or destroying it, stops delivery immediately.
	 *
	 * A returned handle is invalid when the router does not exist yet, which is the case before this
	 * subsystem has initialised. Subscribing that early is a mistake rather than something to retry.
	 */
	FCrowdyServiceRegistry* GetServiceRegistry() const { return ServiceRegistry; }

	FCrowdySubscription Subscribe(TConstArrayView<FCrowdySubscriptionKey> Keys,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const;

	FCrowdySubscription SubscribeToEventPayload(const UScriptStruct* PayloadStruct,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const;

	FCrowdySubscription SubscribeToEventPayload(FCrowdyTypeID TypeID,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const;

	FCrowdySubscription SubscribeToActorUpdatePayload(const UScriptStruct* PayloadStruct,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const;

	FCrowdySubscription SubscribeToActorUpdatePayload(FCrowdyTypeID TypeID,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const;

	FCrowdySubscription SubscribeToAllPayloads(ECrowdyPayloadCategory Category,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const;

	FCrowdySubscription SubscribeToOpcode(ECrowdyMessageType MessageType,
		const FCrowdySubscriptionOptions& Options, FCrowdyDeliveryHandler Handler) const;

	template <typename TMessage>
	FCrowdySubscription SubscribeToOpcode(ECrowdyMessageType MessageType,
		const FCrowdySubscriptionOptions& Options,
		TFunction<void(const TMessage&, const FCrowdyDelivery&)> Handler) const
	{
		if (!ServiceRegistry)
		{
			WarnNoRegistry(Options.SubscriberName);
			return FCrowdySubscription();
		}

		return ServiceRegistry->SubscribeToOpcode<TMessage>(MessageType, Options, MoveTemp(Handler));
	}
	
	void SendMessage(const ICrowdyMessage& Message) const;

private:

	/** Kept out of line so the templated overload above does not pull the log category into every consumer. */
	static void WarnNoRegistry(FName SubscriberName);

	/**
	 * Builds one actor update and sends it. Split out of DispatchActorUpdate so the synchronous caller reaches
	 * it through references rather than through the deferring lambda's copies, which is what makes the id and
	 * the payload a contract: both are read in place, so both must outlive the call and stay unmutated in it.
	 */
	void BuildAndSendActorUpdate(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
	                             const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance,
	                             const FString& InstigatorID, const FInstancedStruct& ActorStatePayload,
	                             FCrowdyClassID ClassID);

	/**
	 * How this game addresses the API, from the developer settings.
	 *
	 * The fallback is the load-bearing part: the gameplay endpoint names one datacenter's instance and is empty
	 * until an app has been resolved, whereas the shared origin is answered by every datacenter. Using it in that
	 * gap is what lets a cold client ask anything at all, including where it should actually be talking.
	 */
	static FCrowdyCppClientConfig ResolveClientConfig();

	UPROPERTY()
	UCrowdyWorkerThreadsSubsystem* WorkerThreadsSubsystem;

	UPROPERTY()
	UCrowdyGameSession* GameSession;
	
	UPROPERTY()
	UCrowdyUDPSubsystem* UdpSubsystem;
	
	UPROPERTY()
	UVoiceChatSubsystem* VoiceChatSubsystem;
	
	UPROPERTY()
	UCrowdyPersistenceSubsystem* PersistenceSubsystem;
	
	// Internal Service References
	FVoiceChatService* VoiceChatService;

	// UDP Messaging
	FCrowdyServiceRegistry* ServiceRegistry;
	FCrowdyMessageParser* Parser;

	/** The ping test response (GENERIC_SPATIAL_1). Released automatically on teardown. */
	FCrowdySubscription PingResponseSubscription;

	FTimerHandle PingMessageTimerHandle;
	FTimerHandle HostPollTimerHandle;
	bool bIsRegistered = false;
	
	UPROPERTY()
	int32 ExpectedActorUpdateStateSize = 300;
	
	UFUNCTION()
	void HandleAuthLogin(FCrowdyAuthResult Result);

	UFUNCTION()
	void HandleAuthRegister(FCrowdyAuthResult Result);

	/** Called by UCrowdyAuthentication::OnLoginFailed to forward failure on OnLogin. */
	UFUNCTION()
	void HandleAuthLoginFailed(FString Message);

	/** Called by UCrowdyAuthentication::OnRegisterFailed to forward failure on OnRegister. */
	UFUNCTION()
	void HandleAuthRegisterFailed(FString Message);

	/** Called by UCrowdyAuthentication::OnSessionRestored requests UDP access with
	 *  the re-minted app token and forwards success on OnLogin. */
	UFUNCTION()
	void HandleAuthSessionRestored(FCrowdyAuthResult Result);

	/** Called by UCrowdyAuthentication::OnAppTokenRefreshed after a token rotation
	 *  re-requests UDP access so the new app token re-assigns the Buddy session. */
	UFUNCTION()
	void HandleAppTokenRefreshed();

	/** Routed (game thread) from the UDP message parser on TOKEN_EXPIRED (error 32);
	 *  asks Authentication to re-mint and re-assign. */
	void HandleTokenExpired();
	
	/** Reports a gate-kept endpoint (the app is full) on OnUDPAddressNotify. Any other endpoint is ignored here.
	 *  Game thread only: it broadcasts on the caller's stack. */
	void HandleUDPAddressNotify(const FCrowdyUDPEndpoint& Endpoint);

	/**
	 * Build and open the routed replication connection, which assigns its own server rather than being handed an
	 * endpoint. Answers through FailUDPAccess when it cannot be opened at all; the outcome of an attempt that does
	 * start arrives later as a connection state change.
	 */
	void OpenRoutedConnection();

	/** The timers a live connection needs, whichever transport carries it. Safe to call again on a reconnect,
	 *  and safe from any thread: each timer registration runs inline on the game thread and is dispatched to
	 *  it otherwise. */
	void StartConnectedSessionWork();

	/** Answers a UDP access request that cannot succeed. Moves the connection out of Connecting so it cannot
	 *  stick there, then reports the failure on OnUDPAddressNotify. Game thread only: it broadcasts on the
	 *  caller's stack, so the game hears the outcomes of a connection attempt in the order they happened. */
	void FailUDPAccess(const FString& Reason) const;

	/** Answers a version query that cannot succeed, with empty versions, so a caller waiting on OnVersionInfo
	 *  is never left without an answer. */
	void FailVersionInfo(const FString& Reason) const;

	/** Answers a teleport check that cannot succeed by denying it, so a caller waiting on OnTeleportPermission
	 *  is never left without an answer. */
	void FailTeleportPermission(const FString& Reason) const;

	bool ValidateVoiceChatSubsystem();
	bool TryLoadConfiguration();

	/** Starts the repeating GameHost poll. Safe to call from any thread: the timer registration runs
	 *  inline when the caller is already on the game thread and is dispatched to it otherwise. Calling
	 *  again while the poll is running does nothing. */
	void StartHostPolling() const;

	/** Cancels the GameHost poll timer. Must be called on the game thread. The poll rides the GameInstance timer
	 *  manager (UWorld::GetTimerManager forwards to it for a Game/PIE world), so it survives level travel and needs
	 *  no per-world re-arm; this teardown on GameInstance shutdown is sufficient. */
	void StopHostPolling();

	/** Fires the GameHost query (called by HostPollTimerHandle). */
	UFUNCTION()
	void PollGameHost() const;

	UFUNCTION()
	void OnUDPTimeout();

	UFUNCTION()
	void OnUDPConnectionSuccessful();

	UFUNCTION()
	void SendPingTestMessage();

	/** Subscribes PingResponseSubscription if it is not already live. Safe to call more than once. */
	void EnsurePingResponseSubscription();
};
