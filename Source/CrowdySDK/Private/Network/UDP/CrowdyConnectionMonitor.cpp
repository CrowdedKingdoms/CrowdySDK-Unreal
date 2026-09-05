// Fill out your copyright notice in the Description page of Project Settings.


#include "Network/UDP/CrowdyConnectionMonitor.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Subsystem/CrowdySDKSubsystem.h"

void UCrowdyConnectionMonitor::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyConnectionMonitor::Deinitialize()
{
	Super::Deinitialize();
	GetWorld()->GetTimerManager().ClearTimer(RetryTimer);
}

void UCrowdyConnectionMonitor::InitConnectionMonitor()
{
	CrowdySDK = GetWorld()->GetGameInstance()->GetSubsystem<UCrowdySDKSubsystem>();
	CrowdyUdp = GetWorld()->GetGameInstance()->GetSubsystem<UCrowdyUDPSubsystem>();
	
	if (!CrowdySDK->OnUDPTimedOut.IsBound())
	{
		CrowdySDK->OnUDPTimedOut.AddDynamic(this, &UCrowdyConnectionMonitor::OnUdpTimeoutDetected);
	}
	
	if (!CrowdyUdp->OnUDPConnectionSuccessful.IsBound())
	{
		CrowdySDK->OnUDPConnectionSuccess.AddDynamic(this, &UCrowdyConnectionMonitor::OnUdpConnectionSuccess);
	}
}

void UCrowdyConnectionMonitor::OnUdpTimeoutDetected()
{
	ReconnectState = ECrowdyReconnectState::Disconnected;
	OnConnectionStateChanged.Broadcast(ReconnectState);

	GetWorld()->GetTimerManager().SetTimer(
		RetryTimer,
		[this]()
		{
			if (AttemptIndex < MaxReconnectAttempts)
			{
				CrowdySDK->StopNetworkOperations();
				ReconnectState = ECrowdyReconnectState::Connecting;
				OnConnectionStateChanged.Broadcast(ReconnectState);
				CrowdySDK->RequestUDPAccess();
				AttemptIndex++;
			}
			else
			{
				ReconnectState = ECrowdyReconnectState::Failed;
				OnConnectionStateChanged.Broadcast(ReconnectState);
				CrowdySDK->StopNetworkOperations();
				GetWorld()->GetTimerManager().ClearTimer(RetryTimer);
			}
		},
		ReconnectAttemptTimeout, // Time interval in seconds
		true // Looping
	);
}

void UCrowdyConnectionMonitor::OnUdpConnectionSuccess()
{
	ReconnectState = ECrowdyReconnectState::Connected;
	OnConnectionStateChanged.Broadcast(ReconnectState);
	GetWorld()->GetTimerManager().ClearTimer(RetryTimer);
	IsUdpMonitoringActive = true;
	AttemptIndex = 0;
}
