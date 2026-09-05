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

	/**
	 * Called once after the backend object is created. Acquire subsystems and read config here.
	 *
	 * Return false when this backend cannot draw anything with what it was given, naming the reason and
	 * the remedy in the log first. The caller refuses a backend that answers false rather than installing
	 * it, so a misconfigured map says so instead of running with every entity tracked and none drawn.
	 */
	virtual bool InitializeBackend(UWorld* World, UCrowdyRenderingBackendConfig* Config) { return true; }

	/** Called on subsystem shutdown. Release any held references here. */
	virtual void DeinitializeBackend() {}

	/**
	 * Whether this backend draws a remote entity as one of a crowd rather than as an actor of its own class.
	 *
	 * Answer true from a backend that represents entities without spawning an actor of the entity's class,
	 * because there is then no actor for that class's Blueprint bodies to run on and an observer runs them on
	 * a shared stand-in instead. Authoring surfaces that only mean something on that representation are shown
	 * exactly where a map profile selects a backend answering true, so a project drawing every map with actors
	 * is not offered options it cannot act on.
	 *
	 * Read off the class default object, from the profile's Backend Class, without creating a backend.
	 */
	virtual bool DrawsEntitiesAsCrowdRows() const { return false; }

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
