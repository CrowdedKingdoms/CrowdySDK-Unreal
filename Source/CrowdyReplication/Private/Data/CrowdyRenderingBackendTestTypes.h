// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyRenderingBackend.h"
#include "Data/CrowdyRenderingBackendConfig.h"
#include "Data/CrowdyRepApplicationPolicy.h"
#include "GameFramework/Actor.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "StructUtils/InstancedStruct.h"
#include "CrowdyRenderingBackendTestTypes.generated.h"

/** A concrete policy that applies nothing, for the cases that ask which policy class was chosen. */
UCLASS()
class UCrowdyInertRepPolicy : public UCrowdyRepApplicationPolicy
{
	GENERATED_BODY()

public:
	virtual bool ExtractFields(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId) override { return true; }
	virtual void ApplyToActor(AActor* Actor, int32 SlotId, int64 RenderTimeMs) override {}
};

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
	TArray<FInstancedStruct> ActivatedStates;
	TArray<UClass*> ActivatedClasses;

	/** Answer IsInstanceActive false, as a backend does when it could not take a rendering resource. */
	bool bReportInactive = false;

	virtual bool InitializeBackend(UWorld* World, UCrowdyRenderingBackendConfig* Config) override
	{
		return true;
	}

	virtual void ActivateInstance(int32 SlotId, const FGuid& UUID, UClass* EntityClass, const FInstancedStruct& InitialState) override
	{
		ActivatedSlots.Add(SlotId);
		ActivatedStates.Add(InitialState);
		ActivatedClasses.Add(EntityClass);
	}

	virtual bool IsInstanceActive(int32 SlotId) const override
	{
		return !bReportInactive;
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

/** A recording backend that re-registers the entity inside ActivateInstance, as the actor pool does. */
UCLASS()
class UCrowdyRegisteringBackend : public UCrowdyRecordingBackend
{
	GENERATED_BODY()

public:

	UPROPERTY()
	TObjectPtr<UCrowdyEntitySubsystem> Entities;

	virtual void ActivateInstance(int32 SlotId, const FGuid& UUID, UClass* EntityClass, const FInstancedStruct& InitialState) override
	{
		Super::ActivateInstance(SlotId, UUID, EntityClass, InitialState);

		// Bounded, so a manager that re-enters shows up as extra activations rather than a stack overflow.
		if (!Entities || ActivatedSlots.Num() > 4) return;

		FCrowdyEntityRecord Record;
		Record.NetID = UUID;
		Record.Role = ECrowdyRole::RemoteProxy;
		Record.Participant = this;
		Entities->UnregisterEntity(UUID);
		Entities->RegisterEntity(Record);
	}
};

/** Records whether collision was on while it was being constructed, which is before any pool policy runs. */
UCLASS()
class ACrowdyPoolCollisionProbeActor : public AActor
{
	GENERATED_BODY()

public:

	bool bCollisionDuringConstruction = true;

	virtual void OnConstruction(const FTransform& Transform) override
	{
		Super::OnConstruction(Transform);
		bCollisionDuringConstruction = GetActorEnableCollision();
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
