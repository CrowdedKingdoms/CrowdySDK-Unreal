// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CrowdyServicesLog.h"
#include "CoreMinimal.h"
#include "Messages/Blueprint/FGameEventNotificationBP.h"
#include "Messages/Actor/FActorUpdateNotificationMessage.h"
#include "Messages/Blueprint/FActorUpdateNotificationBP.h"
#include "Messages/GameObjects/FGameEventNotification.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
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


	void DispatchToBlueprint(const TSharedRef<const ICrowdyMessage, ESPMode::ThreadSafe>& Message) const;

	virtual void BeginDestroy() override;

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

	// Builds one subscription per declared struct/name plus one for the unclaimed-event and
	// unclaimed-actor-update fallbacks this layer's arrays imply. Safe to call more than once:
	// a non-empty Subscriptions array means it already ran.
	void RegisterLayer();

	UPROPERTY()
	UCrowdySDKBridgeSubsystem* SDKBridge;

	TArray<FCrowdySubscription> Subscriptions;
};
