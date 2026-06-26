// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyRenderingBackendConfig.h"
#include "CrowdyActorPoolPolicy.h"
#include "CrowdyRepApplicationPolicy.h"
#include "CrowdyActorPoolBackendConfig.generated.h"

/**
 * Configuration for UCrowdyActorPoolBackend. Set ReplicationPolicyClass to your
 * UCrowdyRepApplicationPolicy subclass. Pools are created lazily per entity class
 * using PoolPolicyClass and DefaultPoolSizePerClass; use PerClassPoolOverrides to
 * pre-warm specific classes with a custom pool size at map load.
 */
UCLASS(Blueprintable, BlueprintType, EditInlineNew, DefaultToInstanced,
	meta=(DisplayName="Actor Pool Backend Config"))
class CROWDYREPLICATION_API UCrowdyActorPoolBackendConfig : public UCrowdyRenderingBackendConfig
{
	GENERATED_BODY()

public:

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Actor Pool Backend")
	TSubclassOf<UCrowdyRepApplicationPolicy> ReplicationPolicyClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Actor Pool Backend")
	TSubclassOf<UCrowdyActorPoolPolicy> PoolPolicyClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Actor Pool Backend", meta=(ClampMin=1))
	int32 DefaultPoolSizePerClass = 8;

	/** Per-class pool size overrides. Classes listed here are also pre-warmed at map load. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Actor Pool Backend")
	TMap<TSoftClassPtr<AActor>, int32> PerClassPoolOverrides;
};
