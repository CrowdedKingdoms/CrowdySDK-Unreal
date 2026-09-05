// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyRenderingBackend.h"
#include "Data/CrowdyRenderingBackendConfig.h"
#include "StructUtils/InstancedStruct.h"
#include "CrowdyRenderingBackendTestTypes.generated.h"

/** A concrete backend config of a type no shipped backend accepts, for the mismatched-config cases. */
UCLASS()
class UCrowdyUnrelatedBackendConfig : public UCrowdyRenderingBackendConfig
{
	GENERATED_BODY()
};

/**
 * A backend that initializes cleanly and records what the manager asked of each slot.
 *
 * The recording is the point: whether a released slot was cleaned up is only visible from the backend's
 * side, and a counter kept beside the manager would still read green if the call itself were dropped.
 */
UCLASS()
class UCrowdyRecordingBackend : public UCrowdyRenderingBackend
{
	GENERATED_BODY()

public:

	TArray<int32> ActivatedSlots;
	TArray<int32> DeactivatedSlots;
	TArray<int32> ExtractedSlots;

	/** What the backend was actually handed, so a test can tell a delivered state from an empty one. */
	TArray<FInstancedStruct> ExtractedStates;

	virtual bool InitializeBackend(UWorld* World, UCrowdyRenderingBackendConfig* Config) override
	{
		return true;
	}

	virtual void ActivateInstance(int32 SlotId, const FGuid& UUID, UClass* EntityClass, const FInstancedStruct& InitialState) override
	{
		ActivatedSlots.Add(SlotId);
	}

	virtual void DeactivateInstance(int32 SlotId, const FGuid& UUID) override
	{
		DeactivatedSlots.Add(SlotId);
	}

	virtual void ExtractUpdate(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId) override
	{
		ExtractedSlots.Add(SlotId);
		ExtractedStates.Add(State);
	}

	virtual void ApplyInterpolation(int32 SlotId, int64 RenderTimeMs) override
	{
	}
};

/** A backend that reports it cannot draw anything with what it was given. */
UCLASS()
class UCrowdyRefusingBackend : public UCrowdyRenderingBackend
{
	GENERATED_BODY()

public:

	virtual bool InitializeBackend(UWorld* World, UCrowdyRenderingBackendConfig* Config) override
	{
		return false;
	}

	virtual void ActivateInstance(int32 SlotId, const FGuid& UUID, UClass* EntityClass, const FInstancedStruct& InitialState) override
	{
	}

	virtual void DeactivateInstance(int32 SlotId, const FGuid& UUID) override
	{
	}

	virtual void ExtractUpdate(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId) override
	{
	}

	virtual void ApplyInterpolation(int32 SlotId, int64 RenderTimeMs) override
	{
	}
};
