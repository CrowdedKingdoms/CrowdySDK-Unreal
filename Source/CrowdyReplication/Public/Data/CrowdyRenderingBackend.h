// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "CrowdyRenderingBackend.generated.h"

class UCrowdyRenderingBackendConfig;

/**
 * Abstract rendering backend for CrowdySDK replicated instances.
 * Implement this to provide a custom rendering strategy (GPU instancing, Niagara, or your own pooling logic).
 * The SDK ships UCrowdyActorPoolBackend as the default implementation.
 */
UCLASS(Abstract, Blueprintable, BlueprintType, EditInlineNew, DefaultToInstanced,
	meta=(DisplayName="Crowdy Rendering Backend"))
class CROWDYREPLICATION_API UCrowdyRenderingBackend : public UObject
{
	GENERATED_BODY()

public:

	/** Called once after the backend object is created. Acquire subsystems and read config here. */
	virtual void InitializeBackend(UWorld* World, UCrowdyRenderingBackendConfig* Config) {}

	/** Called on subsystem shutdown. Release any held references here. */
	virtual void DeinitializeBackend() {}

	/**
	 * A new remote instance has become visible. Acquire whatever rendering resource
	 * represents it (actor from pool, instanced mesh slot, etc.).
	 * SlotId is a stable index managed by CrowdyActorManager for this instance's lifetime.
	 * EntityClass is the resolved actor class from the spawn event. Never null when called.
	 */
	virtual void ActivateInstance(int32 SlotId, const FGuid& UUID, UClass* EntityClass, const FInstancedStruct& InitialState)
		PURE_VIRTUAL(UCrowdyRenderingBackend::ActivateInstance,)

	/**
	 * The remote instance has left. Release the rendering resource and clean up the per-slot state.
	 */
	virtual void DeactivateInstance(int32 SlotId, const FGuid& UUID)
		PURE_VIRTUAL(UCrowdyRenderingBackend::DeactivateInstance,)

	/**
	 * A network update arrived for this slot. Parse the state struct and store it in your
	 * interpolation buffers so ApplyInterpolation can sample it this frame.
	 */
	virtual void ExtractUpdate(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId)
		PURE_VIRTUAL(UCrowdyRenderingBackend::ExtractUpdate,)

	/**
	 * Apply interpolated state to the rendering resource for this slot.
	 * RenderTimeMs is already offset by the interpolation delay.
	 */
	virtual void ApplyInterpolation(int32 SlotId, int64 RenderTimeMs)
		PURE_VIRTUAL(UCrowdyRenderingBackend::ApplyInterpolation,)
};
