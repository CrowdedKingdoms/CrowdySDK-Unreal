// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyConnectionMonitor.generated.h"


class UCrowdySDKSubsystem;
class UCrowdyUDPSubsystem;


UENUM(BlueprintType)
enum class ECrowdyReconnectState  : uint8
{
	Disconnected UMETA(DisplayName="Disconnected"),
	Connecting UMETA(DisplayName="Connecting"),
	Connected UMETA(DisplayName="Connected"),
	Failed UMETA(DisplayName="Failed"),
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnConnectionStatusChange, ECrowdyReconnectState, NewState);


/**
 * 
 */
UCLASS(BlueprintType)
class CROWDYSDK_API UCrowdyConnectionMonitor : public UGameInstanceSubsystem
{
	GENERATED_BODY()
	
public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	
	UPROPERTY(BlueprintAssignable, Category = "CrowdySDK|Connection|Status")
	FOnConnectionStatusChange OnConnectionStateChanged;

	UFUNCTION(BlueprintCallable, Category = "CrowdySDK|Connection|Status")
	void InitConnectionMonitor();
private:
	
	UPROPERTY()
	UCrowdySDKSubsystem* CrowdySDK;
	
	UPROPERTY()
	UCrowdyUDPSubsystem* CrowdyUdp;
	
	FTimerHandle RetryTimer;
	FTimerHandle ReconnectMessageTimer;
	int32 MaxReconnectAttempts = 6;
	int32 AttemptIndex = 0;
	float ReconnectAttemptTimeout = 10.0f;
	ECrowdyReconnectState ReconnectState = ECrowdyReconnectState::Connected;
	bool IsUdpMonitoringActive = false;
	
	
	UFUNCTION()
	void OnUdpTimeoutDetected();
	
	UFUNCTION()
	void OnUdpConnectionSuccess();
};

