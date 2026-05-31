// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "CoreMinimal.h"
#include <chrono>
#include "Subsystems/GameInstanceSubsystem.h"
#include "Sockets.h"
#include "TimerManager.h"
#include "HAL/RunnableThread.h"
#include "Engine/World.h"
#include "Core/UDP/Enums/ECrowdyUDPProtocol.h"
#include "CrowdyUDPSubsystem.generated.h"

struct FUDPAddressNotify;
class FUDPListener;
class UCrowdyGameSession;

/**
 * Observable lifecycle state of the UDP connection.
 * Read this via UCrowdySDKSubsystem::GetUDPConnectionState() to drive UI.
 */
UENUM(BlueprintType)
enum class EUDPConnectionState : uint8
{
	/** Not connected and no reconnect in progress. */
	Disconnected  UMETA(DisplayName = "Disconnected"),
	/** UDP_Access query is in flight or the socket is being initialised. */
	Connecting    UMETA(DisplayName = "Connecting"),
	/** Socket initialised and data packets are being received. */
	Connected     UMETA(DisplayName = "Connected"),
	/** Timeout detected — re-querying the server for a new UDP endpoint. */
	Reconnecting  UMETA(DisplayName = "Reconnecting"),
	/** Server is gatekeeping this client (bGateKeep = true in the response). */
	GateKeep      UMETA(DisplayName = "Gate Kept"),
};


DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FUDPEndpointSet, bool, bSuccess, bool, bGateKeep);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FUDPConnectionSuccessful);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FUDPTimeout);


USTRUCT(BlueprintType)
struct FUDPNetworkStatistics
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats")
	int32 BytesSent = 0;
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats")
	int32 BytesReceived = 0;
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats")
	int32 DatagramsSent = 0;
	
	UPROPERTY(BlueprintReadOnly, Category= "Network Stats")
	int32 DatagramsReceived = 0;
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats")
	int32 MessagesSentPerSecond = 0;
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats")
	int32 MessagesReceivedPerSecond = 0;
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats")
	float SendRecvRatio = 0.0f;
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats")
	int32 TotalClientNotifiesSent = 0;
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats")
	int32 TotalClientNotifiesReceived = 0;
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats", meta=(DisplayName="Dropped Client Notifies"))
	int32 TotalPendingClientNotifies = 0;
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats", meta=(DisplayName="Client Notify Loss Percentage"))
	float ClientNotifyLossPercentage = 0.0f;
	
	UPROPERTY(BlueprintReadOnly, Category = "Network Stats")
	int64 Ping = 0;
	
};


/**
 * 
 */
UCLASS(meta=(DisplayName="Crowdy UDP Subsystem"))
class CROWDYSDK_API UCrowdyUDPSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	UPROPERTY()
	FUDPEndpointSet OnUDPEndpointSet;
	
	UPROPERTY()
	FUDPConnectionSuccessful OnUDPConnectionSuccessful;
	
	UPROPERTY()
	FUDPTimeout OnUDPTimeout;
	
	[[nodiscard]] bool SendMessage(TArray<uint8>&& Message);
	
	void HandleUDPMessage(const uint8* Data, int32 Size);
	
	void OnMessageReceived();
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Network|UDP|Stats", meta=(DisplayName="Get UDP Network Stats"))
	FUDPNetworkStatistics GetUDPNetworkStats() const;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Network|UDP|Stats", meta=(DisplayName="Reset UDP Network Stats"))
	void ResetUDPNetworkStats();
	
	// Call this where the parsing happens
	void IncrementReceivedMessageCount();

	void ToggleUdpEvents(bool bAllow);
	
	void IncrementTotalClientNotifiesReceived();
	
	void UpdatePingTime(const int64 NewPingTime);
	
private:

	friend class UCrowdySDKSubsystem;

	// ── Connection state ───────────────────────────────────────────────────
	/** Atomic so it can be read cheaply from any thread. Cast via enum. */
	std::atomic<uint8> ConnectionState { static_cast<uint8>(EUDPConnectionState::Disconnected) };

	/** Which protocol to use when InitializeUDP is called. Set from Developer Settings. */
	ECrowdyUDPProtocol PreferredProtocol = ECrowdyUDPProtocol::Auto;

	/** Read the current connection state (no game-thread requirement). */
	EUDPConnectionState GetConnectionState() const;

	/** Update the state atom — safe to call from any thread. */
	void SetConnectionState(EUDPConnectionState NewState);

	/** Called by UCrowdySDKSubsystem::TryLoadConfiguration. */
	void SetPreferredProtocol(ECrowdyUDPProtocol Protocol);
	// ──────────────────────────────────────────────────────────────────────

	std::atomic<bool> bIsShuttingDown { false };

	// UDP Operations
	std::atomic<bool> bUDPReady = false;
	std::atomic<bool> bUDPv6Listen = false;
	std::atomic<bool> bUDPConnected = false;
	std::atomic<bool> bUseIPv4 = false;
	std::atomic<bool> bAllowUdpEvents = false;
	
	// Sockets
	FSocket* UDPSocketV6;
	FSocket* UDPSocketV4;
	
	// Network Stats
	std::atomic<int32> ReceivedBytesThisSecond = 0;
	std::atomic<int32> ReceivedDatagramsThisSecond = 0;
	std::atomic<int32> SentBytesThisSecond = 0;
	std::atomic<int32> SentDatagramsThisSecond = 0;
	std::atomic<int32> MessagesReceivedThisSecond = 0;
	std::atomic<int32> MessagesSentThisSecond = 0;
	std::atomic<int32> TotalClientNotifiesSent = 0;
	std::atomic<int32> TotalClientNotifiesReceived = 0;
	std::atomic<int64> PingTime = 0;
	
	// Snapshot values for displaying
	int32 LastSecondReceivedBytes = 0;
	int32 LastSecondReceivedDatagrams = 0;
	int32 LastSecondSentBytes = 0;
	int32 LastSecondSentDatagrams = 0;
	int32 LastSecondMessagesReceived = 0;
	int32 LastSecondMessagesSent = 0;
	
	// Timer Handles
	FTimerHandle UDPStatsTimerHandle;
	FTimerHandle TimeoutCheckTimerHandle;
	
	// Time-Related atomics
	std::atomic<bool> bTimeoutEnabled = false;
	std::atomic<float> TimeoutThresholdSeconds = 5.0f;
	std::atomic<std::chrono::steady_clock::time_point> LastMessageTime;
	
	// Optional Vars for development purposes
	std::atomic<bool> bDiscardReceivedMessages = false;
	
	// References
	UPROPERTY()
	UCrowdyGameSession* GameSession;
	
	// Threading Vars
	FRunnableThread* ListenerThread = nullptr;
	FUDPListener* ListenerRunnable = nullptr;
	
	
	void StartTimeoutMonitoring(float ThresholdSeconds);
	
	void StopTimeoutMonitoring();
	
	void StopUDPOperations()
	{
		StopAllOperations();
	}
	
	void ToggleUDPMessageProcessing()
	{
		bDiscardReceivedMessages = !bDiscardReceivedMessages;
	}
	
	
	
	[[nodiscard]] bool InitializeUDP(const FUDPAddressNotify& UDPAddressNotify);
	
	
	// Timeout Functions
	void CheckForTimeout();
	void StopAllOperations();
	
	// UDP Functions
	[[nodiscard]] bool IsUDPReady() const {return bUDPReady;}
	
	[[nodiscard]] bool InitializeUDPSocket(const FString& IPAddress, const int32 Port);
	
	[[nodiscard]] bool InitializeV4UDPSocket(const FString& IPAddress, const int32 Port);
	
	[[nodiscard]] bool SendUDPv6(const TArray<uint8>& Message);
	
	[[nodiscard]] bool SendUDPv4(const TArray<uint8>& Message);
	
	void StopUDPListener();
	
	void CleanupSockets();
	
	void UpdateUDPStats();
	
};
