// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/GraphQL/Interfaces/ICrowdyQueryReceptionLayer.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"
#include "Shared/Types/Structures/Versioning/FGameVersion.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"  
#include "StructUtils/InstancedStruct.h"// EUDPConnectionState
#include "Subsystem/CrowdyAuthentication.h"
#include "CrowdySDKSubsystem.generated.h"


class UCrowdyPersistenceSubsystem;

// Structs
struct FUDPAddressNotify;
struct FVersionInfoResponse;
struct FGameSessionInfo;
struct FTeleportResponse;

// Interfaces
class ICrowdyQueryRequest;
class ICrowdyQueryTransmissionLayer;
class ICrowdyQueryReceptionLayer;
class ICrowdyTransmissionLayer;
class ICrowdyService;
class ICrowdyReceptionLayer;
class ICrowdyMessage;
class ICrowdyQueryResponse;

// Core Classes
class FCrowdyServiceRegistry;
class FCrowdyMessageParser;
class FMessageBufferPool;
class FCrowdyDataRegistry;
class FCrowdyQueryParser;
class FVoiceChatService;

// Unreal Subsystems
class UCrowdyWorkerThreadsSubsystem;
class UCrowdyQuerySubsystem;
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
class CROWDYSDK_API UCrowdySDKSubsystem : public UGameInstanceSubsystem, public ICrowdyQueryReceptionLayer, public ICrowdyReceptionLayer
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

	/** Dev-bypass sign-in (DEV_AUTH_BYPASS only). Result arrives on OnLogin. */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void DevLogin(const FString Email) const;

	/** Magic-link step 2 — complete sign-in with the one-time token. Result on OnLogin.
	 *  Pair with UCrowdyAuthentication::RequestLoginLink (step 1). */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void CompleteLoginLink(const FString Token) const;

	/** One-call magic-link sign-in: opens a loopback listener, requests the email link, and
	 *  completes automatically when the user clicks it (dev short-circuits via devToken). Result
	 *  on OnLogin. */
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

	// Re-reads the management/game endpoints from UCrowdySDKDeveloperSettings and pushes
	// them into the query subsystem. Lets a CrowdyStudio Config Sync take effect on a
	// running PIE session without restarting it.
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Network")
	void ReloadEndpointsFromSettings();




	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void SetGameSessionInfo(const FGameSessionInfo GameSessionInfo);
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication", meta=(DisplayName="Request UDP Access"))
	void RequestUDPAccess() const;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication")
	void RequestVersionInfo() const;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Authentication", meta=(DeprecatedFunction))
	void SetQueryEndpoint(const FString InEndpoint) const;
	
	// With these:
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Configuration")
	void SetManagementApiUrl(const FString InUrl) const;

	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Configuration")
	void SetGameApiUrl(const FString InHttpUrl) const;
	
	/**
	 * Returns the current UDP connection state. Poll this to drive connection
	 * indicators in your UI — no delegates or event subscriptions required.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "CrowdySDK|Connection",
		meta=(DisplayName="Get UDP Connection State"))
	EUDPConnectionState GetUDPConnectionState() const;

	/**
	 * @deprecated Timeout monitoring now starts automatically after the UDP
	 * socket is initialised. Configure the threshold in Project Settings →
	 * Plugins → Crowdy SDK → UDP Timeout (seconds).
	 *
	 * This function still works as a manual override (e.g. to use a different
	 * threshold at runtime), but you no longer need to call it.
	 */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection",
		meta=(DisplayName="Start UDP Timeout Monitoring (Deprecated)",
		      DeprecatedFunction,
		      DeprecationMessage="Timeout monitoring is automatic. Set the UDP Timeout from the CrowdyStudio console (Project page, Connection section) instead."))
	void StartUDPTimeoutMonitoring(const float ThresholdSeconds = 30.0f);

	/**
	 * @deprecated Timeout monitoring is stopped automatically when the
	 * subsystem shuts down or a reconnect cycle begins.
	 */
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection",
		meta=(DisplayName="Stop UDP Timeout Monitoring (Deprecated)",
		      DeprecatedFunction,
		      DeprecationMessage="Timeout monitoring is managed automatically by the SDK."))
	void StopUDPTimeoutMonitoring();

	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection")
	void StopNetworkOperations() const;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection")
	void ToggleNetworkMessageProcessing() const;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection", meta=(DisplayName="Trigger UDP Heartbeat"))
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
	
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Reception Layer")
	void DeregisterAllReceptionLayers();
	
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Subsystem")
	void SetExpectedActorUpdateStateSize(const int32 InSize) const;
	
	UFUNCTION(BlueprintCallable, Category="CrowdySDK|Replication|Actor Updates")
	void DispatchActorUpdate(const int64 ChunkX, const int64 ChunkY, const int64 ChunkZ,
	                         const ECrowdyDecayRate DecayRate, const ECrowdyReplicationDistance ReplicationDistance,
	                         const FString& InstigatorID, UPARAM(ref) const FInstancedStruct& ActorStatePayload, bool bAsync = false);
	
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

	/** Actor-to-actor send (SINGLE_ACTOR_MESSAGE): same payload as a game event, but the server
	 *  delivers it only to the client that owns TargetActorID. UUID = TargetActorID, chunk = the
	 *  target's chunk; distance/decay are unused. */
	void DispatchSingleActorMessage_Internal(int64 ChunkX, int64 ChunkY, int64 ChunkZ,
	                                          const FGuid& TargetActorID, FInstancedStruct EventPayload, bool bAsync);

	void RegisterReceptionLayer(ICrowdyReceptionLayer* Layer) const;
	void RegisterQueryReceptionLayer(ICrowdyQueryReceptionLayer* LayerToRegister) const;
	bool IsLayerRegistered(const ICrowdyReceptionLayer* Layer) const;
	
	void SendMessage(const ICrowdyMessage& Message) const;

	void ExecuteQuery(ICrowdyQueryRequest& Query) const;
	
	inline void SetTransmissionLayer(ICrowdyTransmissionLayer* Layer);
	inline void SetQueryTransmissionLayer(ICrowdyQueryTransmissionLayer* Layer);

	virtual void OnResponseReceived(TSharedPtr<ICrowdyQueryResponse> Response) override;
	virtual TArray<EQueryResponseType> GetSupportedResponseType() const override; 
	
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) override;
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const override;

	
private:

	void ApplyEndpointsFromSettings();

	UPROPERTY()
	UCrowdyWorkerThreadsSubsystem* WorkerThreadsSubsystem;
	
	UPROPERTY()
	UCrowdyQuerySubsystem* QuerySubsystem;
	
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
	FMessageBufferPool* BufferPool;
	ICrowdyTransmissionLayer* TransmissionLayer;

	// GraphQL Query Execution
	FCrowdyDataRegistry* DataRegistry;
	FCrowdyQueryParser* QueryParser;
	ICrowdyQueryTransmissionLayer* QueryTransmissionLayer;
	
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

	/** Called by UCrowdyAuthentication::OnSessionRestored — requests UDP access with
	 *  the re-minted app token and forwards success on OnLogin. */
	UFUNCTION()
	void HandleAuthSessionRestored(FCrowdyAuthResult Result);

	/** Called by UCrowdyAuthentication::OnAppTokenRefreshed after a token rotation —
	 *  re-requests UDP access so the new app token re-assigns the Buddy session. */
	UFUNCTION()
	void HandleAppTokenRefreshed();

	/** Routed (game thread) from the UDP message parser on TOKEN_EXPIRED (error 32);
	 *  asks Authentication to re-mint and re-assign. */
	void HandleTokenExpired();
	
	void HandleUDPAddressNotify(const FUDPAddressNotify& UDPAddressNotify); // non-const: manages ping timer
	void HandleVersionInfoResponse(const FVersionInfoResponse& VersionInfoResponse) const;
	void HandleTeleportPermissionResponse(const FTeleportResponse& TeleportResponse) const;
	bool ValidateVoiceChatSubsystem();
	bool TryLoadConfiguration();

	/** Starts the repeating GameHost poll. Safe to call from any thread —
	 *  dispatches the timer registration to the game thread internally. */
	void StartHostPolling() const;

	/** Cancels the GameHost poll timer. Must be called on the game thread. */
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
};
