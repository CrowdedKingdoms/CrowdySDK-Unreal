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
	
	CrowdyUdp->ToggleUdpEvents(true); // Set to true
}

void UCrowdyConnectionMonitor::OnUdpTimeoutDetected()
{
	ReconnectState = ECrowdyReconnectState::Disconnected;
	OnConnectionStateChanged.Broadcast(ReconnectState);
	CrowdyUdp->ToggleUdpEvents(false); // set to false

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

				GetWorld()->GetTimerManager().SetTimer(
					ReconnectMessageTimer,
					[this]()
					{
						CrowdyUdp->ToggleUdpEvents(true);
					},
					3.0f, // Time interval in seconds
					false // Looping
				);
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
	CrowdyUdp->ToggleUdpEvents(true);
	GetWorld()->GetTimerManager().ClearTimer(RetryTimer);
	GetWorld()->GetTimerManager().ClearTimer(ReconnectMessageTimer);
	CrowdySDK->StartUDPTimeoutMonitoring(10.0f);
	IsUdpMonitoringActive = true;
	AttemptIndex = 0;
}
