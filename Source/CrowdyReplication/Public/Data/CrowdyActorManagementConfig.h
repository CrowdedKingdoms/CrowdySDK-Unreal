// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
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

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker"))
	float ActorTimeoutThreshold = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 1))
	int32 MaxTrackedActors = 1000;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 1))
	int32 MaxUpdatesPerBatch = 100;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker", ClampMin = 0.001f, ClampMax = 0.1f))
	float MaxBatchWaitTime = 0.005f;

	/** The rendering backend to use for this world. Defaults to UCrowdyActorPoolBackend. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy SDK|Actor Management Config", meta = (EditCondition = "bUseCrowdyActorTracker"))
	TSubclassOf<UCrowdyRenderingBackend> BackendClass;

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
