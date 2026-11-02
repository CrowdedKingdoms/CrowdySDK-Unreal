// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyRenderingBackend.h"
#include "CrowdyMassRenderingBackend.generated.h"

class UCrowdyMassEntitySubsystem;
class UCrowdyMassRenderingBackendConfig;
class UCrowdyEntitySubsystem;

/**
 * Mass-only rendering backend. Every remote Crowdy instance is rendered as a lightweight
 * Mass entity instead of a pooled actor. Lifecycle is driven by UCrowdyActorManager (the
 * same path that drives UCrowdyActorPoolBackend) and delegated to UCrowdyMassEntitySubsystem,
 * which owns the Mass entities and their transform interpolation.
 *
 * The resolved actor class passed to ActivateInstance never builds anything here: the Mass entity is
 * built from the configured Mass Entity Config (see UCrowdyMassRenderingBackendConfig and
 * Project Settings > Plugins > Crowdy Mass), not from an actor class. It is still passed on to
 * UCrowdyMassEntitySubsystem, which keeps it as this entity's CrowdyState routing class (see
 * ICrowdyEntitySubscriber), so a class carrying its own CrowdyState-replicated properties reaches
 * this entity even though it never spawns an actor of that class. Per-frame interpolation is handled
 * inside the Mass pipeline by UCrowdyRemoteTransformProcessor, so ApplyInterpolation is intentionally a
 * no-op.
 *
 * Configure via UCrowdyMassRenderingBackendConfig.
 */
UCLASS(Blueprintable, BlueprintType, EditInlineNew, DefaultToInstanced,
	meta=(DisplayName="Crowdy Mass Rendering Backend"))
class CROWDYMASS_API UCrowdyMassRenderingBackend : public UCrowdyRenderingBackend
{
	GENERATED_BODY()

public:

	virtual bool InitializeBackend(UWorld* World, UCrowdyRenderingBackendConfig* Config) override;
	virtual void DeinitializeBackend() override;

	// A remote entity is a Mass entity here and no actor of its class is ever spawned for it, which is what
	// makes a class's own Blueprint bodies run on a shared stand-in on an observer.
	virtual bool DrawsEntitiesAsCrowdRows() const override { return true; }

	virtual void ActivateInstance(int32 SlotId, const FGuid& UUID, UClass* EntityClass, const FInstancedStruct& InitialState) override;
	virtual void DeactivateInstance(int32 SlotId, const FGuid& UUID) override;
	virtual void ExtractUpdate(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId) override;
	virtual void ApplyInterpolation(int32 SlotId, int64 RenderTimeMs) override;
	virtual void TakeSpawnData(const FGuid& UUID, const FInstancedStruct& InitialState) override;
	virtual bool ReleaseDestroyedInstance(int32 SlotId, const FGuid& UUID) override;

	// Spawn data held for a row that does not exist yet. Spawn events are forgeable, so the wait is bounded in count
	// and, once that is reached, in time: past the tracker's 12-second timeout an entity's first update is not coming.
	static constexpr int32 MaxPendingSpawnData = 1024;
	static constexpr double PendingSpawnDataSeconds = 15.0;

#if WITH_DEV_AUTOMATION_TESTS
	// Test seam: binds a slot to an entity without a live activation, so a case can drive ExtractUpdate and
	// measure what one received payload costs on the real path rather than on the door below it.
	void BindSlotForTest(int32 SlotId, const FGuid& UUID, UCrowdyMassEntitySubsystem* Subsystem);
	int32 GetPendingSpawnDataCountForTest() const { return PendingSpawnData.Num(); }
#endif

private:

	UPROPERTY()
	TObjectPtr<UCrowdyMassEntitySubsystem> MassSubsystem;

	// Used only to skip the locally-owned entity (it already has its real representation).
	UPROPERTY()
	TObjectPtr<UCrowdyEntitySubsystem> EntitySubsystem;

	UPROPERTY()
	TObjectPtr<UCrowdyMassRenderingBackendConfig> MassConfig;

	/** SlotId -> entity UUID. ExtractUpdate only receives a SlotId, so the mapping lives here. */
	TArray<FGuid> SlotToUUID;

	// A spawn event usually arrives before the entity's first update, so before its row exists.
	struct FHeldSpawnData
	{
		FInstancedStruct State;
		double ExpiresAt = 0.0;
	};
	TMap<FGuid, FHeldSpawnData> PendingSpawnData;

	// Rows that took their spawn data, so a repeated or forged spawn event is not applied over newer state.
	TSet<FGuid> SpawnDataApplied;
	bool bReportedPendingSpawnDataFull = false;

	void EnsureSlotCapacity(int32 SlotId);
};
