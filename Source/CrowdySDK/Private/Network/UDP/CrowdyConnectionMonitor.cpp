// Fill out your copyright notice in the Description page of Project Settings.


#include "Network/UDP/CrowdyConnectionMonitor.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "TimerManager.h"

void UCrowdyConnectionMonitor::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

void UCrowdyConnectionMonitor::Deinitialize()
{
	Super::Deinitialize();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RetryTimer);
	}
}

void UCrowdyConnectionMonitor::InitConnectionMonitor()
{
	UGameInstance* Instance = GetGameInstance();
	AttachTo(Instance ? Instance->GetSubsystem<UCrowdySDKSubsystem>() : nullptr,
		Instance ? Instance->GetSubsystem<UCrowdyUDPSubsystem>() : nullptr);
}

void UCrowdyConnectionMonitor::AttachTo(UCrowdySDKSubsystem* Sdk, UCrowdyUDPSubsystem* Udp)
{
	CrowdySDK = Sdk;
	CrowdyUdp = Udp;
	if (!CrowdySDK)
	{
		return;
	}
	// The SDK subsystem and game code bind these delegates too; only this monitor's own binding must be unique.
	CrowdySDK->OnUDPTimedOut.AddUniqueDynamic(this, &UCrowdyConnectionMonitor::OnUdpTimeoutDetected);
	CrowdySDK->OnUDPConnectionSuccess.AddUniqueDynamic(this, &UCrowdyConnectionMonitor::OnUdpConnectionSuccess);
}

void UCrowdyConnectionMonitor::OnUdpTimeoutDetected()
{
	ReconnectState = ECrowdyReconnectState::Disconnected;
	OnConnectionStateChanged.Broadcast(ReconnectState);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	World->GetTimerManager().SetTimer(
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
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RetryTimer);
	}
	IsUdpMonitoringActive = true;
	AttemptIndex = 0;
}
