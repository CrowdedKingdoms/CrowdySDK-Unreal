#pragma once
#include "CoreMinimal.h"
#include "CrowdyActorPoolPolicy.h"
#include "FCrowdyPoolConfig.generated.h"

USTRUCT(BlueprintType)
struct FCrowdyPoolConfig
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Actor Pool")
	TSubclassOf<AActor> ActorClass;
	
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Actor Pool")
	TSubclassOf<UCrowdyActorPoolPolicy> PoolPolicyClass;
	
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Actor Pool")
	int32 PoolSize = 64;
};