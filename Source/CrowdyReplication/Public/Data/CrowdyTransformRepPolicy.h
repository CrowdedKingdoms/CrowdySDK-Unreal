// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyRepApplicationPolicy.h"
#include "Data/TInterpolatedField.h"
#include "CrowdyTransformRepPolicy.generated.h"

/**
 * The policy the actor pool backend uses when a profile names none: applies the SDK's FCrowdyActorState
 * (location and rotation) to a pooled actor. Each slot keeps a ring of timestamped samples and renders the
 * pair that brackets the render time, lerping the location and slerping the rotation, with extrapolation
 * past the newest sample capped at 0.2 s. Subclass UCrowdyRepApplicationPolicy instead when your executor
 * sends a state struct of your own.
 */
UCLASS(meta=(DisplayName="Crowdy Transform Replication Policy"))
class CROWDYREPLICATION_API UCrowdyTransformRepPolicy : public UCrowdyRepApplicationPolicy
{
	GENERATED_BODY()

public:
	virtual void OnInstanceActivated_Implementation(AActor* Actor, const FInstancedStruct& InitialState) override;
	virtual bool ExtractFields(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId) override;
	virtual void ApplyToActor(AActor* Actor, int32 SlotId, int64 RenderTimeMs) override;
	virtual void OnInstanceDeactivated(int32 SlotId) override;

private:
	// Indexed by the SlotId the pool assigns.
	TArray<TInterpolatedField<FVector>> Positions;
	TArray<TInterpolatedField<FRotator>> Rotations;

	void EnsureSlot(int32 SlotId);
};
