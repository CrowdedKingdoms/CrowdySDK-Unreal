// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "CrowdyRepApplicationPolicy.generated.h"

/**
 * 
 */
UCLASS(Abstract, Blueprintable, EditInlineNew, DefaultToInstanced, meta=(DisplayName="Crowdy Replication Application Policy"))
class CROWDYSDK_API UCrowdyRepApplicationPolicy : public UObject
{
	GENERATED_BODY()
	
public:
	
	/**
	* Called once when an instance slot is activated.
	* Use this to set the initial state on the actor.
	*/
	UFUNCTION(BlueprintNativeEvent, Category="Replication Policy")
	void OnInstanceActivated(AActor* Actor, const FInstancedStruct& InitialState);
	virtual void OnInstanceActivated_Implementation(AActor* Actor, const FInstancedStruct& InitialState) {}

	/**
	 * Extract interpolatable fields from a raw state struct.
	 * Returns false if the struct type is wrong, the subsystem will skip the update.
	 */
	virtual bool ExtractFields(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId) PURE_VIRTUAL(UReplicationPolicy::ExtractFields, return false;);

	/**
	 * Apply interpolated data to an actor this frame.
	 * SlotId is passed so you can read back from your own SoA buffers.
	 */
	virtual void ApplyToActor(AActor* Actor, int32 SlotId, int64 RenderTimeMs) PURE_VIRTUAL(UReplicationPolicy::ApplyToActor,);

	/**
	 * Called when a slot is deactivated (UUID left).
	 * Clean up any per-slot state here.
	 */
	virtual void OnInstanceDeactivated(int32 SlotId) {}

	/** How many slots to pre-allocate. Override to match your expected player count. */
	virtual int32 GetExpectedSlotCount() const { return 64; }
};
