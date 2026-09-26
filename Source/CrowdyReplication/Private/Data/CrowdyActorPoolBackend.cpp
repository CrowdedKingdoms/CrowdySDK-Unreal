// Fill out your copyright notice in the Description page of Project Settings.

#include "Data/CrowdyActorPoolBackend.h"
#include "CrowdyReplicationLog.h"

#include "Data/CrowdyActorPoolBackendConfig.h"
#include "Data/CrowdyRepApplicationPolicy.h"
#include "Data/CrowdyTransformRepPolicy.h"
#include "Data/FCrowdyPoolConfig.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/Subsystems/CrowdyActorPoolSubsystem.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

UCrowdyActorPoolBackendConfig* UCrowdyActorPoolBackend::ResolveConfig(UCrowdyRenderingBackendConfig* Config, UObject* Outer)
{
	// A config of another backend's class is an authoring mistake, logged rather than ensured so a packaged
	// build says the same thing an editor build does. No config at all is what the shipped default profile
	// carries, and it runs on a transient one so a project that configured nothing still draws its entities.
	if (UCrowdyActorPoolBackendConfig* PoolConfig = Cast<UCrowdyActorPoolBackendConfig>(Config))
	{
		return PoolConfig;
	}
	if (Config)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyActorPoolBackend]: Backend Config is a %s, but this backend needs a CrowdyActorPoolBackendConfig ")
			TEXT("to know what to spawn. Set Backend Config on the map profile to a Crowdy Actor Pool Backend Config."),
			*Config->GetClass()->GetName());
		return nullptr;
	}
	return NewObject<UCrowdyActorPoolBackendConfig>(Outer);
}

UClass* UCrowdyActorPoolBackend::ResolvePolicyClass(const UCrowdyActorPoolBackendConfig* Config)
{
	UClass* PolicyClass = Config ? Config->ReplicationPolicyClass.Get() : nullptr;
	if (IsValid(PolicyClass))
	{
		return PolicyClass;
	}
	UE_CLOG(CrowdyReplicationTrace::Pool(), LogCrowdyReplication, Log,
		TEXT("[CrowdyActorPoolBackend]: No Replication Policy Class on '%s'; using CrowdyTransformRepPolicy."),
		*GetNameSafe(Config));
	return UCrowdyTransformRepPolicy::StaticClass();
}

bool UCrowdyActorPoolBackend::InitializeBackend(UWorld* World, UCrowdyRenderingBackendConfig* Config)
{
	PoolConfig = ResolveConfig(Config, this);
	if (!PoolConfig)
	{
		return false;
	}

	ActorPool = World->GetSubsystem<UCrowdyActorPoolSubsystem>();
	if (!ensureMsgf(IsValid(ActorPool), TEXT("[CrowdyActorPoolBackend]: CrowdyActorPoolSubsystem not found.")))
		return false;

	EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!ensureMsgf(IsValid(EntitySubsystem), TEXT("[CrowdyActorPoolBackend]: CrowdyEntitySubsystem not found.")))
		return false;

	Policy = NewObject<UCrowdyRepApplicationPolicy>(this, ResolvePolicyClass(PoolConfig));

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
		Cfg.MaxPoolSize     = PoolConfig->MaxPoolSizePerClass;
		ActorPool->RegisterPool(Cfg);
	}

	return true;
}

void UCrowdyActorPoolBackend::DeinitializeBackend()
{
	Policy         = nullptr;
	ActorPool      = nullptr;
	EntitySubsystem = nullptr;
	PoolConfig     = nullptr;
	SlotActors.Empty();
	UnpooledSlots.Empty();
	OwnerProxySlots.Empty();
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

	// A previous actor still here means a missed deactivation; only a record naming that actor is dropped.
	if (!bOwnerEntity && SlotActors[SlotId].IsValid())
	{
		const bool bClaimed = IsSlotActorClaimed(SlotId);
		if (EntitySubsystem->FindEntity(UUID) == SlotActors[SlotId].Get())
			EntitySubsystem->UnregisterEntity(UUID);
		ReleaseSlotActor(SlotId, bClaimed);
	}

	// Secured before the orphan (the actor FinishRemoteSpawn made) is touched, so a refusal never costs it.
	AActor* Actor = ActorPool->AcquireActor(EntityClass);

	const FCrowdyEntityRecord* OrphanRecord = bOwnerEntity ? nullptr : EntitySubsystem->FindRecord(UUID);
	const bool bHasOrphanRecord = OrphanRecord != nullptr;
	AActor* Orphan = OrphanRecord ? OrphanRecord->GetActor() : nullptr;

	if (!Actor && !IsValid(Orphan))
	{
		UE_CLOG(CrowdyReplicationTrace::Pool(), LogCrowdyReplication, Log,
			TEXT("[CrowdyActorPoolBackend]: No pool actor for %s: entity %s not activated, retried on its next update."),
			*EntityClass->GetName(), *UUID.ToString());
		return;
	}

	// At the cap the orphan is adopted with its own record, and destroyed rather than pooled on release.
	const bool bAdopted = !Actor;
	const bool bReplacedOrphan = !bAdopted && IsValid(Orphan);
	if (bAdopted)
	{
		Actor = Orphan;
	}
	else if (bHasOrphanRecord)
	{
		if (bReplacedOrphan)
			Orphan->Destroy();
		EntitySubsystem->UnregisterEntity(UUID);
	}

	if (!bOwnerEntity && !bAdopted)
	{
		// Build the entity record. Inherit OwnerID/ClassID if the spawn event already
		// populated a record (spawn event arrived before pool activation).
		FCrowdyEntityRecord Record;
		Record.NetID = UUID;
		Record.Role  = ECrowdyRole::RemoteProxy;
		Record.Participant = Actor;

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
	UnpooledSlots[SlotId] = bAdopted;
	OwnerProxySlots[SlotId] = bOwnerEntity;

	UE_CLOG(CrowdyReplicationTrace::Pool(), LogCrowdyReplication, Log,
		TEXT("[CrowdyActorPoolBackend]: Activated slot %d: entity %s, class %s, location %s, orphan replaced %s, adopted %s."),
		SlotId, *UUID.ToString(), *EntityClass->GetName(),
		InitialState.GetPtr<FCrowdyActorState>() ? *InitialState.GetPtr<FCrowdyActorState>()->Location.ToString() : TEXT("unknown"),
		bReplacedOrphan ? TEXT("yes") : TEXT("no"),
		bAdopted ? TEXT("yes") : TEXT("no"));
}

void UCrowdyActorPoolBackend::DeactivateInstance(int32 SlotId, const FGuid& UUID)
{
	const bool bHasSlot = SlotActors.IsValidIndex(SlotId);
	const AActor* SlotActor = bHasSlot ? SlotActors[SlotId].Get() : nullptr;
	const bool bClaimed = bHasSlot && IsSlotActorClaimed(SlotId);

	// Owner entities never got a RemoteProxy record registered, so do not remove their Owner record.
	if (IsValid(EntitySubsystem))
	{
		const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(UUID);
		const bool bOwnerRecord = Record && Record->Role == ECrowdyRole::Owner;
		AActor* Unadopted = Record && Record->Role == ECrowdyRole::RemoteProxy ? Record->GetActor() : nullptr;

		// A spawn-event actor this slot never adopted would otherwise outlive its record.
		if (IsValid(Unadopted) && Unadopted != SlotActor)
			Unadopted->Destroy();
		if (!bOwnerRecord)
			EntitySubsystem->UnregisterEntity(UUID);
	}

	if (bHasSlot)
		ReleaseSlotActor(SlotId, bClaimed);

	if (IsValid(Policy))
		Policy->OnInstanceDeactivated(SlotId);
}

bool UCrowdyActorPoolBackend::IsInstanceActive(const int32 SlotId) const
{
	return SlotActors.IsValidIndex(SlotId) && SlotActors[SlotId].IsValid() && IsSlotActorClaimed(SlotId);
}

bool UCrowdyActorPoolBackend::IsSlotActorClaimed(const int32 SlotId) const
{
	return OwnerProxySlots[SlotId] || (IsValid(EntitySubsystem) && EntitySubsystem->FindEntityID(SlotActors[SlotId].Get()).IsValid());
}

void UCrowdyActorPoolBackend::ReleaseSlotActor(const int32 SlotId, const bool bClaimed)
{
	AActor* Actor = SlotActors[SlotId].Get();
	const bool bUnpooled = UnpooledSlots[SlotId];
	SlotActors[SlotId] = nullptr;
	UnpooledSlots[SlotId] = false;
	OwnerProxySlots[SlotId] = false;

	// An actor whose record someone else removed is theirs to destroy; the pool prunes it once it is gone.
	if (!IsValid(Actor) || !bClaimed) return;

	if (bUnpooled)
	{
		Actor->Destroy();
		return;
	}

	if (IsValid(ActorPool))
		ActorPool->ReleaseActor(Actor);
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
	if (SlotId < SlotActors.Num()) return;

	SlotActors.SetNum(SlotId + 1);
	UnpooledSlots.SetNum(SlotId + 1, false);
	OwnerProxySlots.SetNum(SlotId + 1, false);
}

void UCrowdyActorPoolBackend::EnsurePoolForClass(UClass* ActorClass)
{
	if (ActorPool->HasPool(ActorClass)) return;

	// Check PerClassPoolOverrides for a configured size; fall back to default.
	const int32* Override = IsValid(PoolConfig) ? PoolConfig->PerClassPoolOverrides.Find(TSoftClassPtr<AActor>(ActorClass)) : nullptr;
	const int32 Size = Override ? *Override : (IsValid(PoolConfig) ? PoolConfig->DefaultPoolSizePerClass : 8);

	FCrowdyPoolConfig Cfg;
	Cfg.ActorClass      = ActorClass;
	Cfg.PoolPolicyClass  = IsValid(PoolConfig) ? PoolConfig->PoolPolicyClass : nullptr;
	Cfg.PoolSize         = Size;
	if (IsValid(PoolConfig))
		Cfg.MaxPoolSize  = PoolConfig->MaxPoolSizePerClass;

	ActorPool->RegisterPool(Cfg);
}
