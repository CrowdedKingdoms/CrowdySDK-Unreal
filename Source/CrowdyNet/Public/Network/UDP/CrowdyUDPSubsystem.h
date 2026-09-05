// Fill out your copyright notice in the Description page of Project Settings.

#pragma once
#include "CoreMinimal.h"
#include <atomic>
#include "Subsystems/GameInstanceSubsystem.h"
#include "Engine/World.h"
#include "CrowdyUDPSubsystem.generated.h"

/**
 * Observable lifecycle state of the UDP connection.
 * Read this via UCrowdySDKSubsystem::GetUDPConnectionState() to drive UI.
 */
UENUM(BlueprintType)
enum class EUDPConnectionState : uint8
{
	/** Not connected and no reconnect in progress. */
	Disconnected  UMETA(DisplayName = "Disconnected"),
	/** The connection is being opened. */
	Connecting    UMETA(DisplayName = "Connecting"),
	/** The connection is up and carrying traffic. */
	Connected     UMETA(DisplayName = "Connected"),
	/** The connection lost its server and is re-assigning one. */
	Reconnecting  UMETA(DisplayName = "Reconnecting"),
	/** Server is gatekeeping this client (bGateKeep = true in the response). */
	GateKeep      UMETA(DisplayName = "Gate Kept"),
};


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

	UPROPERTY(BlueprintReadOnly, Category = "Network Stats", meta=(DisplayName="Total Messages Sent"))
	int64 TotalMessagesSent = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Network Stats", meta=(DisplayName="Total Messages Received"))
	int64 TotalMessagesReceived = 0;

};

/** One second of transport activity, as differences against the previous reading. */
struct FCrowdyTransportSample
{
	int32 BytesSent = 0;
	int32 BytesReceived = 0;
	int32 DatagramsSent = 0;
	int32 DatagramsReceived = 0;
	int32 MessagesSent = 0;
};

/**
 *
 */
UCLASS(meta=(DisplayName="Crowdy UDP Subsystem"))
class CROWDYNET_API UCrowdyUDPSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	UPROPERTY()
	FUDPConnectionSuccessful OnUDPConnectionSuccessful;
	
	UPROPERTY()
	FUDPTimeout OnUDPTimeout;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Network|UDP|Stats", meta=(DisplayName="Get UDP Network Stats"))
	FUDPNetworkStatistics GetUDPNetworkStats() const;
	
	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Network|UDP|Stats", meta=(DisplayName="Reset UDP Network Stats"))
	void ResetUDPNetworkStats();
	
	// Call this where the parsing happens
	void IncrementReceivedMessageCount();

	void IncrementTotalClientNotifiesReceived();

	/** Add a batch of sent client notifications. Batched because sends are counted off the game thread. */
	void AddTotalClientNotifiesSent(int64 Count);

	/**
	 * Publish one second's worth of transport activity: how many bytes and datagrams moved in each direction,
	 * and how many messages were sent. Read back through GetUDPNetworkStats().
	 */
	void ReportTransportSample(const FCrowdyTransportSample& Sample);

	/**
	 * Report that the connection is up: moves the state to Connected and tells everything waiting on the
	 * connection, which is what joins the reliable-RPC channels. Broadcast on every transition rather than once,
	 * since a re-assignment is a new session.
	 *
	 * Game thread only.
	 */
	void MarkRoutedConnectionUp();

	/** True when received messages are being discarded for development. */
	[[nodiscard]] bool IsDiscardingReceivedMessages() const { return bDiscardReceivedMessages; }

	/** True once teardown has begun, after which nothing should be dispatched. */
	[[nodiscard]] bool IsShuttingDown() const { return bIsShuttingDown; }
	
	void UpdatePingTime(const int64 NewPingTime);
	
	/** Read the current connection state (no game-thread requirement). */
	EUDPConnectionState GetConnectionState() const;

	// Called by UCrowdySDKSubsystem to drive UDP lifecycle.
	void SetConnectionState(EUDPConnectionState NewState);
	void StopUDPOperations() { StopAllOperations(); }
	void ToggleUDPMessageProcessing() { bDiscardReceivedMessages = !bDiscardReceivedMessages; }

private:

	/** Atomic so it can be read cheaply from any thread. Cast via enum. */
	std::atomic<uint8> ConnectionState { static_cast<uint8>(EUDPConnectionState::Disconnected) };

	std::atomic<bool> bIsShuttingDown { false };

	// Network Stats
	std::atomic<int32> MessagesReceivedThisSecond = 0;
	std::atomic<int32> TotalClientNotifiesSent = 0;
	std::atomic<int32> TotalClientNotifiesReceived = 0;
	std::atomic<int64> PingTime = 0;
	std::atomic<int64> TotalMessagesSent = 0;
	std::atomic<int64> TotalMessagesReceived = 0;
	
	// Snapshot values for displaying
	int32 LastSecondReceivedBytes = 0;
	int32 LastSecondReceivedDatagrams = 0;
	int32 LastSecondSentBytes = 0;
	int32 LastSecondSentDatagrams = 0;
	int32 LastSecondMessagesReceived = 0;
	int32 LastSecondMessagesSent = 0;

	// Optional Vars for development purposes
	std::atomic<bool> bDiscardReceivedMessages = false;

	void StopAllOperations();

};
