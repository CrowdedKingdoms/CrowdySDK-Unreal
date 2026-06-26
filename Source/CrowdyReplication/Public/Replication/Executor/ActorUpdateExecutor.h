// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "ActorUpdateExecutor.generated.h"

/**
 * Builds the FInstancedStruct snapshot the AutoReplicator sends every interval.
 * The parameter is the UCrowdyEntityComponent driving replication — use
 * GetOwner() on it to reach the replicated actor.
 */
UCLASS(Blueprintable, BlueprintType, EditInlineNew, Abstract)
class CROWDYREPLICATION_API UActorUpdateExecutor : public UObject
{
	GENERATED_BODY()
public:

	UFUNCTION(BlueprintNativeEvent, Category = "Crowdy SDK|Actor Update Executor")
	FInstancedStruct GetActorState(const UActorComponent* UpdateComponent) const;

	/**
	 * The struct type GetActorState snapshots into. Declaring it here is what
	 * registers the type for wire serialization — no struct annotation needed.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Crowdy SDK|Actor Update Executor")
	UScriptStruct* GetStateStruct() const;
	virtual UScriptStruct* GetStateStruct_Implementation() const { return nullptr; }
};
