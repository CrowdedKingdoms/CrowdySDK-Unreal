// Fill out your copyright notice in the Description page of Project Settings.

#include "Data/CrowdyActorPoolBackend.h"
#include "CrowdyReplicationLog.h"

#include "Data/CrowdyActorPoolBackendConfig.h"
#include "Data/CrowdyRepApplicationPolicy.h"
#include "Data/FCrowdyPoolConfig.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Subsystems/CrowdyActorPoolSubsystem.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

void UCrowdyActorPoolBackend::InitializeBackend(UWorld* World, UCrowdyRenderingBackendConfig* Config)
{
	PoolConfig = Cast<UCrowdyActorPoolBackendConfig>(Config);
	if (!ensureMsgf(IsValid(PoolConfig), TEXT("[CrowdyActorPoolBackend]: Expected UCrowdyActorPoolBackendConfig but got a different type or null.")))
		return;

	ActorPool = World->GetSubsystem<UCrowdyActorPoolSubsystem>();
	if (!ensureMsgf(IsValid(ActorPool), TEXT("[CrowdyActorPoolBackend]: CrowdyActorPoolSubsystem not found.")))
		return;

	EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!ensureMsgf(IsValid(EntitySubsystem), TEXT("[CrowdyActorPoolBackend]: CrowdyEntitySubsystem not found.")))
		return;

	if (!ensureMsgf(IsValid(PoolConfig->ReplicationPolicyClass.Get()),
		TEXT("[CrowdyActorPoolBackend]: ReplicationPolicyClass is not set in UCrowdyActorPoolBackendConfig.")))
		return;

	Policy = NewObject<UCrowdyRepApplicationPolicy>(this, PoolConfig->ReplicationPolicyClass.Get());

	// Pre-warm pools for classes listed in PerClassPoolOverrides so their actors are
	// ready before the first entity of that class arrives, avoiding a frame spike.
	for (const auto& [SoftClass, PoolSize] : PoolConfig->PerClassPoolOverrides)
	{
		UClass* ActorClass = SoftClass.Get();
		if (!ActorClass)
		{
			UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyActorPoolBackend]: PerClassPoolOverrides entry '%s' not loaded at backend init — skipping pre-warm."), *SoftClass.ToString());
			continue;
		}

		FCrowdyPoolConfig Cfg;
		Cfg.ActorClass     = ActorClass;
		Cfg.PoolPolicyClass = PoolConfig->PoolPolicyClass;
		Cfg.PoolSize        = PoolSize;
		ActorPool->RegisterPool(Cfg);
	}
}

void UCrowdyActorPoolBackend::DeinitializeBackend()
{
	Policy         = nullptr;
	ActorPool      = nullptr;
	EntitySubsystem = nullptr;
	PoolConfig     = nullptr;
	SlotActors.Empty();
}

void UCrowdyActorPoolBackend::ActivateInstance(int32 SlotId, const FGuid& UUID, UClass* EntityClass, const FInstancedStruct& InitialState)
{
	if (!EntityClass)
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[CrowdyActorPoolBackend]: ActivateInstance called with null EntityClass for %s."), *UUID.ToString());
		return;
	}
	if (!IsValid(ActorPool) || !IsValid(EntitySubsystem)) return;

	EnsureSlotCapacity(SlotId);

	const FCrowdyEntityRecord* ExistingRecord = EntitySubsystem->FindRecord(UUID);
	const bool bOwnerEntity = ExistingRecord && ExistingRecord->Role == ECrowdyRole::Owner;

	EnsurePoolForClass(EntityClass);

	if (!bOwnerEntity)
	{
		// Release a previous actor in this slot if a prior deactivation was missed.
		if (AActor* Previous = SlotActors[SlotId].Get())
		{
			EntitySubsystem->UnregisterEntity(UUID);
			ActorPool->ReleaseActor(Previous);
			SlotActors[SlotId] = nullptr;
		}

		// If the entity was spawned by FinishRemoteSpawn before pool activation, destroy that orphan.
		if (const FCrowdyEntityRecord* Orphan = EntitySubsystem->FindRecord(UUID))
		{
			if (AActor* OldActor = Orphan->Actor.Get())
				OldActor->Destroy();
			EntitySubsystem->UnregisterEntity(UUID);
		}
	}

	AActor* Actor = ActorPool->AcquireActor(EntityClass);
	if (!Actor)
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyActorPoolBackend]: Pool exhausted for %s — entity %s not activated."), *EntityClass->GetName(), *UUID.ToString());
		return;
	}

	if (!bOwnerEntity)
	{
		// Build the entity record. Inherit OwnerID/ClassID if the spawn event already
		// populated a record (spawn event arrived before pool activation).
		FCrowdyEntityRecord Record;
		Record.NetID = UUID;
		Record.Role  = ECrowdyRole::RemoteProxy;
		Record.Actor = Actor;

		if (const FCrowdyEntityRecord* SpawnRecord = EntitySubsystem->FindRecord(UUID))
		{
			Record.OwnerID = SpawnRecord->OwnerID;
			Record.ClassID = SpawnRecord->ClassID;
		}

		EntitySubsystem->RegisterEntity(Record);

		// Sync the entity component's fields so GetNetID()/GetRole() work on the actor.
		// AssignPooledIdentity (not InitIdentity) because pre-warmed pool actors have
		// already begun play; the RegisterEntity above is the backend-owned record.
		if (UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>())
			Component->AssignPooledIdentity(UUID, Record.OwnerID, ECrowdyRole::RemoteProxy, Record.ClassID);
	}

	SlotActors[SlotId] = Actor;
}

void UCrowdyActorPoolBackend::DeactivateInstance(int32 SlotId, const FGuid& UUID)
{
	// Owner entities never got a RemoteProxy record registered — don't remove their Owner record.
	if (IsValid(EntitySubsystem))
	{
		const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(UUID);
		if (!Record || Record->Role != ECrowdyRole::Owner)
			EntitySubsystem->UnregisterEntity(UUID);
	}

	AActor* Actor = SlotActors.IsValidIndex(SlotId) ? SlotActors[SlotId].Get() : nullptr;

	if (IsValid(ActorPool) && IsValid(Actor))
		ActorPool->ReleaseActor(Actor);

	if (SlotActors.IsValidIndex(SlotId))
		SlotActors[SlotId] = nullptr;

	if (IsValid(Policy))
		Policy->OnInstanceDeactivated(SlotId);
}

void UCrowdyActorPoolBackend::ExtractUpdate(const FInstancedStruct& State, int64 ServerTimestampMs, int32 SlotId)
{
	if (IsValid(Policy))
		Policy->ExtractFields(State, ServerTimestampMs, SlotId);
}

void UCrowdyActorPoolBackend::ApplyInterpolation(int32 SlotId, int64 RenderTimeMs)
{
	if (!IsValid(Policy)) return;
	if (!SlotActors.IsValidIndex(SlotId)) return;

	AActor* Actor = SlotActors[SlotId].Get();
	if (!IsValid(Actor)) return;

	Policy->ApplyToActor(Actor, SlotId, RenderTimeMs);
}

void UCrowdyActorPoolBackend::EnsureSlotCapacity(int32 SlotId)
{
	if (SlotId >= SlotActors.Num())
		SlotActors.SetNum(SlotId + 1);
}

void UCrowdyActorPoolBackend::EnsurePoolForClass(UClass* ActorClass)
{
	const FSoftObjectPath ClassPath(ActorClass->GetPathName());
	const TSoftClassPtr<AActor> SoftClass(ActorClass);

	// Check PerClassPoolOverrides for a configured size; fall back to default.
	const int32* Override = IsValid(PoolConfig) ? PoolConfig->PerClassPoolOverrides.Find(SoftClass) : nullptr;
	const int32 Size = Override ? *Override : (IsValid(PoolConfig) ? PoolConfig->DefaultPoolSizePerClass : 8);

	FCrowdyPoolConfig Cfg;
	Cfg.ActorClass      = ActorClass;
	Cfg.PoolPolicyClass  = IsValid(PoolConfig) ? PoolConfig->PoolPolicyClass : nullptr;
	Cfg.PoolSize         = Size;

	// RegisterPool is idempotent — silently returns if pool for ActorClass already exists.
	ActorPool->RegisterPool(Cfg);
}
