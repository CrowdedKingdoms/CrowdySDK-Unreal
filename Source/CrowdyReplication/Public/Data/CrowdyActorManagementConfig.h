// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyActorPoolBackend.h"
#include "CrowdyRenderingBackend.h"
#include "CrowdyRenderingBackendConfig.h"
#include "Engine/DataAsset.h"
#include "CrowdyActorManagementConfig.generated.h"



USTRUCT(BlueprintType, Category = "Crowdy SDK|Data")
struct FCrowdyActorManagementConfigStruct
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config")
	bool bUseCrowdyActorTracker = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker"))
	bool bDispatchUpdatesOnGameThread = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker"))
	bool bEnableOwnerTracking = true;

	// Seconds of silence before an entity is dropped on a guess. The server announces a real departure about
	// five seconds after the last update, so this is the fallback for a departure that never arrives rather
	// than the usual way an entity leaves. Twelve seconds matches the reaper the other Crowdy SDKs use. The
	// sweep that enforces it runs on its own shorter period, so the actual wait stays near this value.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker"))
	float ActorTimeoutThreshold = 12.0f;

	/** Seconds of silence before a Game Kit starter's actor, such as a mob, leaves. Its hub shows it several times a second while anyone is near and stops when it goes down, so this is how fast a downed mob leaves. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 0.25))
	float KitActorTimeout = 1.0f;

	/**
	 * How many network-received actors may be tracked at once. Ids past this are not tracked at all
	 * until an existing one times out, so the surplus simply does not appear.
	 *
	 * It is a bound on forged input rather than a performance setting: lookups cost the same whether the
	 * set is empty or full. It caps how much memory a peer inventing actor ids can make a client hold, so
	 * set it above the largest crowd the game intends to show rather than near it. Locally spawned actors
	 * do not count against it.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 1))
	int32 MaxTrackedActors = 4096;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 1))
	int32 MaxUpdatesPerBatch = 100;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 0.001f, ClampMax = 0.1f))
	float MaxBatchWaitTime = 0.005f;

	/**
	 * How remote entities are drawn on this map. The actor-pool backend it starts on spawns pooled actors of
	 * the entity's own class; point it at another UCrowdyRenderingBackend subclass to draw them some other way.
	 *
	 * A profile that already names a backend keeps it: this default only fills in a profile that never chose
	 * one.
	 *
	 * Backend Config below is optional for the actor-pool backend, which runs on default settings when it is
	 * empty. A config of the wrong type, or a backend that cannot initialize, is refused with a warning
	 * rather than installed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker"))
	TSubclassOf<UCrowdyRenderingBackend> BackendClass = UCrowdyActorPoolBackend::StaticClass();

	/** Backend-specific settings. Set this to the matching config type for your chosen BackendClass. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Instanced, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker"))
	TObjectPtr<UCrowdyRenderingBackendConfig> BackendConfig;
};


/**
 * 
 */
UCLASS(BlueprintType, Category = "Crowdy SDK|Data", meta = (DisplayName = "Crowdy Actor Management Config"))
class CROWDYREPLICATION_API UCrowdyActorManagementConfig : public UDataAsset
{
	GENERATED_BODY()
	
public:
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config")
	FCrowdyActorManagementConfigStruct Config;
	
};
