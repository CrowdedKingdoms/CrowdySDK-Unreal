// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "ActorUpdateExecutor.generated.h"

class UActorUpdateExecutor;

/**
 * Which entry point an executor's state snapshot should be taken through.
 *
 * GetActorState is a BlueprintNativeEvent, and its generated thunk already forwards straight to
 * GetActorState_Implementation for an executor that does not override it, so what a direct call skips is
 * the resolution in front of that: an FName-keyed FindFunctionChecked under the class's function-map lock,
 * on every call. Resolving that answer once per executor class is what takes the lookup out of the loop.
 */
namespace CrowdyActorUpdateExecutor
{
	/** True when GetActorState is not overridden in Blueprint. A null executor answers false. */
	CROWDYREPLICATION_API bool AnswersStateNatively(const UActorUpdateExecutor* Executor);

	/** The predicate the answer above turns on, split out so both of its answers can be stated as a test. */
	CROWDYREPLICATION_API bool IsNativeStateFunction(const UFunction* StateFunction);
}

/**
 * Builds the FInstancedStruct snapshot the AutoReplicator sends every interval.
 * The parameter is the UCrowdyEntityComponent driving replication: use
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
	 * registers the type for wire serialization: no struct annotation needed.
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Crowdy SDK|Actor Update Executor")
	UScriptStruct* GetStateStruct() const;
	virtual UScriptStruct* GetStateStruct_Implementation() const { return nullptr; }
};

/**
 * The state struct the SDK ships, so a project gets a replicated actor without authoring one.
 *
 * Deliberately just a transform. Two things a reader might expect are absent on purpose:
 *
 * Velocity is NOT here. It is worth carrying only quantized, which is a wire-format break that is not
 * yet justified: the receive drain's two budgets were measured co-binding, and nothing yet says what
 * share of per-message cost is payload size. A raw FVector would cost 24 bytes on every update from
 * every entity in the project to carry something nothing reads. The framing tolerates APPENDED fields,
 * so adding it later is free, while shipping it early is not.
 *
 * View-state bytes (a pose, a stance) are NOT here either. Those are defined by whichever renderer
 * consumes them, they are matched by FIELD NAME through reflection rather than by struct type, and the
 * SDK core does not depend on any renderer. A game that wants them authors its own struct, which stays
 * fully supported: this default removes the obligation, not the option.
 */
USTRUCT(BlueprintType, meta=(DisplayName="Crowdy Actor State"))
struct FCrowdyActorState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Actor State")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Actor State")
	FRotator Rotation = FRotator::ZeroRotator;
};

/**
 * The executor a Dynamic entity uses when none is assigned. Snapshots the owning actor's transform
 * into FCrowdyActorState.
 *
 * Before this existed, a UCrowdyEntityComponent set to Dynamic with no executor logged an error and
 * replicated nothing, so the minimum setup for a replicated actor was a component, an executor class
 * and a state struct. It is now the component alone.
 *
 * This is only safe because class identity travels on every update. A shared default struct used by
 * every entity in a project would previously have made them all resolve to one class, since the
 * receiver derived the class from the struct: the defect amplified rather than fixed.
 */
UCLASS(Blueprintable, BlueprintType, EditInlineNew,
	meta=(DisplayName="Crowdy Default Actor Update Executor"))
class CROWDYREPLICATION_API UCrowdyDefaultActorUpdateExecutor : public UActorUpdateExecutor
{
	GENERATED_BODY()

public:

	virtual FInstancedStruct GetActorState_Implementation(const UActorComponent* UpdateComponent) const override;
	virtual UScriptStruct* GetStateStruct_Implementation() const override { return FCrowdyActorState::StaticStruct(); }
};
