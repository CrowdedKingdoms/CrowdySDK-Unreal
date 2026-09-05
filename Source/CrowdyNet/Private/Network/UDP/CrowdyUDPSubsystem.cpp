// Fill out your copyright notice in the Description page of Project Settings.


#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "CrowdyNetLog.h"

void UCrowdyUDPSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UWorld* World = GetWorld();
	if (!World || !World->GetGameInstance())
	{
		UE_LOG(LogCrowdyNet, Warning, TEXT("UDP Subsystem init skipped — no GameInstance."));
		return;
	}

	UE_CLOG(CrowdyNetTrace::Net(), LogCrowdyNet, Log, TEXT("UDP Subsystem Initialized."))
}

void UCrowdyUDPSubsystem::Deinitialize()
{
	// Signal all in-flight AsyncTasks to abort before we tear anything down
	bIsShuttingDown = true;

	Super::Deinitialize();
}


FUDPNetworkStatistics UCrowdyUDPSubsystem::GetUDPNetworkStats() const
{
	FUDPNetworkStatistics Stats;
	Stats.BytesSent = LastSecondSentBytes;
	Stats.BytesReceived = LastSecondReceivedBytes;
	Stats.DatagramsSent = LastSecondSentDatagrams;
	Stats.DatagramsReceived = LastSecondReceivedDatagrams;
	Stats.MessagesSentPerSecond = LastSecondMessagesSent;
	Stats.MessagesReceivedPerSecond = LastSecondMessagesReceived;
	Stats.TotalClientNotifiesSent = TotalClientNotifiesSent;
	Stats.TotalClientNotifiesReceived = TotalClientNotifiesReceived;
	Stats.TotalMessagesSent = TotalMessagesSent.load();
	Stats.TotalMessagesReceived = TotalMessagesReceived.load();
	Stats.Ping = PingTime;
	Stats.TotalPendingClientNotifies = TotalClientNotifiesSent.load() - TotalClientNotifiesReceived.load();
	
	if (Stats.BytesReceived > 0)
		Stats.SendRecvRatio = static_cast<float>(Stats.BytesSent) / static_cast<float>(Stats.BytesReceived);
	
	if (Stats.TotalClientNotifiesSent > 0)
		Stats.ClientNotifyLossPercentage = ((Stats.TotalClientNotifiesSent - Stats.TotalClientNotifiesReceived) /
			static_cast<float>(Stats.TotalClientNotifiesSent)) * 100.0f;
	
	return Stats;
}

void UCrowdyUDPSubsystem::ResetUDPNetworkStats()
{
	LastSecondSentBytes = 0;
	LastSecondReceivedBytes = 0;
	LastSecondSentDatagrams = 0;
	LastSecondReceivedDatagrams = 0;
	LastSecondMessagesSent = 0;
	LastSecondMessagesReceived = 0;
	TotalClientNotifiesReceived = 0;
	TotalClientNotifiesSent = 0;
	TotalMessagesSent = 0;
	TotalMessagesReceived = 0;
}

void UCrowdyUDPSubsystem::MarkRoutedConnectionUp()
{
	SetConnectionState(EUDPConnectionState::Connected);
	OnUDPConnectionSuccessful.Broadcast();
}

void UCrowdyUDPSubsystem::IncrementReceivedMessageCount()
{
	++MessagesReceivedThisSecond;
	++TotalMessagesReceived;
}

void UCrowdyUDPSubsystem::IncrementTotalClientNotifiesReceived()
{
	++TotalClientNotifiesReceived;
}

void UCrowdyUDPSubsystem::AddTotalClientNotifiesSent(const int64 Count)
{
	if (Count > 0)
	{
		TotalClientNotifiesSent += static_cast<int32>(FMath::Min<int64>(Count, MAX_int32));
	}
}

void UCrowdyUDPSubsystem::ReportTransportSample(const FCrowdyTransportSample& Sample)
{
	LastSecondSentBytes = Sample.BytesSent;
	LastSecondReceivedBytes = Sample.BytesReceived;
	LastSecondSentDatagrams = Sample.DatagramsSent;
	LastSecondReceivedDatagrams = Sample.DatagramsReceived;
	LastSecondMessagesSent = Sample.MessagesSent;

	// Accumulated rather than assigned: ResetUDPNetworkStats() is callable from Blueprint at any time, and an
	// assignment here would undo that reset on the very next report.
	TotalMessagesSent += Sample.MessagesSent;

	// The only place this drains. IncrementReceivedMessageCount() only ever adds to it.
	LastSecondMessagesReceived = MessagesReceivedThisSecond.exchange(0);
}

void UCrowdyUDPSubsystem::UpdatePingTime(const int64 NewPingTime)
{
	PingTime = NewPingTime;
}

void UCrowdyUDPSubsystem::StopAllOperations()
{
	UE_CLOG(CrowdyNetTrace::Net(), LogCrowdyNet, Log, TEXT("UDP: stopping all operations"));

	// Reports the session as down. The connection itself is owned elsewhere and is left alone, so a recovery
	// already under way is not interrupted by a caller asking the UI to show a disconnect.
	SetConnectionState(EUDPConnectionState::Disconnected);
}

EUDPConnectionState UCrowdyUDPSubsystem::GetConnectionState() const
{
	return static_cast<EUDPConnectionState>(ConnectionState.load(std::memory_order_relaxed));
}

void UCrowdyUDPSubsystem::SetConnectionState(const EUDPConnectionState NewState)
{
	ConnectionState.store(static_cast<uint8>(NewState), std::memory_order_relaxed);
}