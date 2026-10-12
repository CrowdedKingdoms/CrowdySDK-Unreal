// Fill out your copyright notice in the Description page of Project Settings.

#include "Backend/CrowdyMassRenderingBackend.h"

#include "Backend/CrowdyMassRenderingBackendConfig.h"
#include "CrowdyMassLog.h"
#include "Engine/World.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

bool UCrowdyMassRenderingBackend::InitializeBackend(UWorld* World, UCrowdyRenderingBackendConfig* Config)
{
	if (!ensureMsgf(IsValid(World), TEXT("[CrowdyMassRenderingBackend]: InitializeBackend called with a null world.")))
		return false;

	// Config is optional: a null or mismatched config just means "use the project-wide Crowdy Mass settings".
	MassConfig = Cast<UCrowdyMassRenderingBackendConfig>(Config);

	MassSubsystem = World->GetSubsystem<UCrowdyMassEntitySubsystem>();
	if (!IsValid(MassSubsystem))
	{
		UE_LOG(LogCrowdyMass, Warning,
			TEXT("[CrowdyMassRenderingBackend]: This world has no UCrowdyMassEntitySubsystem, so no crowd can be drawn here. ")
			TEXT("It exists only on maps whose profile selects the Mass backend; set the Backend Class on this map's profile to the Mass backend."));
		return false;
	}

	// Optional: only used to detect the locally-owned entity. Tolerate its absence.
	EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();

	if (MassConfig)
		MassSubsystem->ConfigureOverrides(MassConfig->RemoteEntityConfigOverride, MassConfig->SmoothingDurationOverride,
			MassConfig->PerClassEntityConfigs);

	return true;
}

void UCrowdyMassRenderingBackend::DeinitializeBackend()
{
	// Destroy whatever this backend spawned (matters on a runtime backend swap; harmless on teardown).
	if (IsValid(MassSubsystem))
	{
		for (const FGuid& UUID : SlotToUUID)
		{
			if (UUID.IsValid())
				MassSubsystem->DestroyRemoteEntity(UUID);
		}
	}

	SlotToUUID.Empty();
	MassSubsystem = nullptr;
	EntitySubsystem = nullptr;
	MassConfig = nullptr;
}

void UCrowdyMassRenderingBackend::ActivateInstance(int32 SlotId, const FGuid& UUID, UClass* EntityClass, const FInstancedStruct& InitialState)
{
	if (!IsValid(MassSubsystem))
		return;

	EnsureSlotCapacity(SlotId);

	// Keep the slot mapping consistent even when we skip the spawn below, so later
	// ExtractUpdate/DeactivateInstance calls for this slot resolve to the UUID and no-op cleanly.
	SlotToUUID[SlotId] = UUID;

	// The locally-owned entity is already represented by its real pawn/actor, so by default we never
	// spawn a Mass proxy for it. bRenderLocalOwner overrides this for single-client visualization testing
	// (you'll then see your real pawn AND an overlapping Mass duplicate).
	const bool bRenderOwner = MassConfig && MassConfig->bRenderLocalOwner;
	if (!bRenderOwner && IsValid(EntitySubsystem))
	{
		const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(UUID);
		if (Record && Record->Role == ECrowdyRole::Owner)
		{
			UE_LOG(LogCrowdyMass, Verbose, TEXT("[CrowdyMassRenderingBackend]: Skipping Mass proxy for locally-owned entity %s (enable bRenderLocalOwner on the backend config to render it)."), *UUID.ToString());
			return;
		}
	}

	FVector Location;
	FQuat Rotation;
	float SmoothingDuration;
	UCrowdyMassEntitySubsystem::TryExtractTransformState(InitialState, Location, Rotation, SmoothingDuration);

	// EntityClass is the locally-resolved class UCrowdyActorManager::ResolveEntityClass produced for this
	// entity, and it now decides two things. It is what UCrowdyMassEntitySubsystem answers with as this
	// entity's ICrowdyEntitySubscriber class, which is how a CrowdyState delta with no actor to
	// decode onto still finds the right rep layout for this id. And it selects the Mass Entity Config
	// when the backend config lists one for this class, which is what lets two kinds of crowd entity
	// look different; an unlisted class still builds from the single configured default, as before.
	MassSubsystem->SpawnRemoteEntity(UUID, FTransform(Rotation, Location), InitialState, EntityClass);

	UE_LOG(LogCrowdyMass, Verbose, TEXT("[CrowdyMassRenderingBackend]: Activated slot %d for entity %s (resolved class %s, used only for CrowdyState routing; the Mass entity itself renders from the Mass Entity Config)."),
		SlotId, *UUID.ToString(), *GetNameSafe(EntityClass));
}

void UCrowdyMassRenderingBackend::DeactivateInstance(int32 SlotId, const FGuid& UUID)
{
	if (IsValid(MassSubsystem))
		MassSubsystem->DestroyRemoteEntity(UUID);

	if (SlotToUUID.IsValidIndex(SlotId))
		SlotToUUID[SlotId] = FGuid{};
}

void UCrowdyMassRenderingBackend::ExtractUpdate(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId)
{
	if (!IsValid(MassSubsystem) || !SlotToUUID.IsValidIndex(SlotId))
		return;

	const FGuid& UUID = SlotToUUID[SlotId];
	if (!UUID.IsValid())
		return;

	// One door for both halves of the payload, so the UUID is resolved once rather than once per half.
	// No-op if the entity was owner-skipped, which is what a false return means.
	MassSubsystem->ApplyRemoteEntityUpdate(UUID, State, ServerTimestampMs);
}

#if WITH_DEV_AUTOMATION_TESTS
void UCrowdyMassRenderingBackend::BindSlotForTest(const int32 SlotId, const FGuid& UUID, UCrowdyMassEntitySubsystem* Subsystem)
{
	MassSubsystem = Subsystem;
	EnsureSlotCapacity(SlotId);
	SlotToUUID[SlotId] = UUID;
}
#endif

void UCrowdyMassRenderingBackend::ApplyInterpolation(int32 /*SlotId*/, int64 /*RenderTimeMs*/)
{
	// Intentionally empty: UCrowdyRemoteTransformProcessor advances interpolation every frame
	// inside the Mass pipeline (PrePhysics), so there is nothing to apply per-slot here.
}

void UCrowdyMassRenderingBackend::EnsureSlotCapacity(int32 SlotId)
{
	if (SlotId >= SlotToUUID.Num())
		SlotToUUID.SetNum(SlotId + 1);
}
