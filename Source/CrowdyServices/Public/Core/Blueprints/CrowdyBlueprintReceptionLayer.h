// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CrowdyServicesLog.h"
#include "CoreMinimal.h"
#include "FCrowdyBPLayerBridge.h"
#include "Messages/Blueprint/FGameEventNotificationBP.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Blueprint/FActorUpdateNotificationBP.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "UObject/Object.h"
#include "CrowdyBlueprintReceptionLayer.generated.h"


DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameEventNotificationReceived, const FGameEventNotificationBP&, Notification);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnActorUpdateReceived, const FActorUpdateNotificationBP&, Notification);

/**
 * 
 */

UCLASS(BlueprintType, Blueprintable, meta=(DisplayName="Crowdy Blueprint Reception Layer"))
class CROWDYSERVICES_API UCrowdyBlueprintReceptionLayer : public UObject
{
	GENERATED_BODY()
	
public:
	
	UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = "Crowdy SDK|Reception Layer|Actor Updates")
	FOnGameEventNotificationReceived OnEventNotificationReceived;

	UPROPERTY(BlueprintAssignable, BlueprintCallable, Category = "Crowdy SDK|Reception Layer|Game Events")
	FOnActorUpdateReceived OnActorUpdateReceived;

	UPROPERTY(EditDefaultsOnly, Category = "Crowdy SDK|Reception Layer")
	TArray<ECrowdyMessageType> SupportedResponseTypes;
	
	UPROPERTY(EditDefaultsOnly, Category = "Crowdy SDK|Reception Layer")
	TArray<FName> SupportedActorUpdateTypes;

	/** Event payload structs this layer claims; empty = receive unclaimed events only. */
	UPROPERTY(EditDefaultsOnly, Category = "Crowdy SDK|Reception Layer")
	TArray<TObjectPtr<UScriptStruct>> SupportedEvents;

public:
	
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Reception Layer")
	static UCrowdyBlueprintReceptionLayer* CreateAndRegisterLayer(UObject* WorldContextObject,
	                                                              TSubclassOf<UCrowdyBlueprintReceptionLayer> Class);
	
	
	void DispatchToBlueprint(const TSharedRef<ICrowdyMessage, ESPMode::ThreadSafe>& Message) const;
	
	virtual void BeginDestroy() override;
	
	/* TODO: Add in QOL updates */
	/*
	UFUNCTION(BlueprintCallable, Category = "Crowdy")
	void UnregisterFromRegistry()
	{
		if (!Bridge.IsValid()) return;

		FCrowdyServiceRegistry::Get()->DeregisterReceptionLayer(Bridge.Get());
		Bridge.Reset();
	}
	*/
private:
	
	void Initialize(UCrowdySDKBridgeSubsystem* InBridgeSub)
	{
		if (HasAnyFlags(RF_ClassDefaultObject))
		{
			UE_LOG(LogCrowdyServices, Warning, TEXT("[Blueprint Reception Layer]: Failed to register. Returning early."));
			return;
		}
		SDKBridge = InBridgeSub;
	}

	void RegisterLayer()
	{
		if (Bridge.IsValid()) return; // already registered
		Bridge = MakeShared<FCrowdyBPLayerBridge>(this);
		if (SDKBridge && SDKBridge->ServiceRegistry)
			SDKBridge->ServiceRegistry->RegisterReceptionLayer(Bridge.Get());
	}

	UPROPERTY()
	UCrowdySDKBridgeSubsystem* SDKBridge;
	
	TSharedPtr<FCrowdyBPLayerBridge> Bridge;
};
