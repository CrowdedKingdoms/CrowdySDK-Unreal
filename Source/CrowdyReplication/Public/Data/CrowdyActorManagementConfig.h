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

	/** Off: this map receives no other entity's movement and draws no remote entity. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Entities", meta = (DisplayName = "Draw Remote Entities"))
	bool bUseCrowdyActorTracker = true;

	/** Also broadcast each update batch to the tracker's OnUpdatesGameThread, for a Blueprint listener. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Remote Entities", meta = (EditCondition = "bUseCrowdyActorTracker"))
	bool bDispatchUpdatesOnGameThread = false;

	/** Track this client's own entity as the server echoes it back, which proves the server is receiving you. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Remote Entities", meta = (EditCondition = "bUseCrowdyActorTracker"))
	bool bEnableOwnerTracking = true;

	/** Silence before a remote entity is dropped on a guess; the server normally announces a departure about 5 s after the last update. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Remote Entities", meta = (EditCondition = "bUseCrowdyActorTracker", Units = "s", DisplayName = "Actor Timeout"))
	float ActorTimeoutThreshold = 12.0f;

	/** Silence before a Game Kit starter's actor, such as a downed mob, leaves. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Remote Entities", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 0.25, Units = "s"))
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
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Remote Entities", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 1))
	int32 MaxTrackedActors = 4096;

	/** How many updates for not-yet-seen entities are batched off the game thread before they are handed over. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Remote Entities", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 1))
	int32 MaxUpdatesPerBatch = 100;

	/** The longest a batch of updates for not-yet-seen entities waits before it is handed over. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AdvancedDisplay, Category = "Remote Entities", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 0.001f, ClampMax = 0.1f, Units = "s"))
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
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Entities", meta = (EditCondition = "bUseCrowdyActorTracker"))
	TSubclassOf<UCrowdyRenderingBackend> BackendClass = UCrowdyActorPoolBackend::StaticClass();

	/** Backend-specific settings. Set this to the matching config type for your chosen BackendClass. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Instanced, Category = "Remote Entities", meta = (EditCondition = "bUseCrowdyActorTracker"))
	TObjectPtr<UCrowdyRenderingBackendConfig> BackendConfig;

	/** How far behind the server's clock remote entities are drawn: longer hides late updates, and below one send interval (100 ms at 10 Hz) they overshoot and snap back. Mass-drawn rows ignore it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Entities", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 0, UIMax = 500, ForceUnits = "ms", DisplayName = "Interpolation Delay"))
	int32 InterpolationDelayMs = 100;
};


/** Deprecated: nothing reads this asset at runtime. Set these values on a Crowdy Map Profile's Remote Entities. */
UCLASS(BlueprintType, HideDropdown, Category = "Crowdy SDK|Data", meta = (DisplayName = "Crowdy Actor Management Config (Deprecated)"))
class CROWDYREPLICATION_API UCrowdyActorManagementConfig : public UDataAsset
{
	GENERATED_BODY()
	
public:
	
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Remote Entities")
	FCrowdyActorManagementConfigStruct Config;
	
};
