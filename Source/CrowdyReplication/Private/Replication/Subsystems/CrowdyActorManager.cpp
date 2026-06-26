// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/Subsystems/CrowdyActorManager.h"
#include "CrowdyReplicationLog.h"

#include "Data/CrowdyRenderingBackend.h"
#include "Data/CrowdyRenderingBackendConfig.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/UCrowdyClassRegistry.h"

void UCrowdyActorManager::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UWorld* World = GetWorld();
	check(IsValid(World))

	if (!IsValid(World))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Manager]: Invalid world"));
		return;
	}

	ActorTracker = World->GetSubsystem<UCrowdyActorTracker>();
	check(IsValid(ActorTracker))

	if (!IsValid(ActorTracker))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Manager]: Invalid actor tracker"));
		return;
	}

	EntitySubsystem = Collection.InitializeDependency<UCrowdyEntitySubsystem>();
	if (!IsValid(EntitySubsystem))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Manager]: Failed to get CrowdyEntitySubsystem"));
		return;
	}

	if (!LoadConfig())
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Manager]: Failed to load config due to incorrect params or disabled by choice."));
		return;
	}

	ActorTracker->OnRemoteEntityAppeared.AddDynamic(this, &UCrowdyActorManager::HandleActorSpawned);
	ActorTracker->OnRemoteEntityTimedOut.AddDynamic(this, &UCrowdyActorManager::HandleActorDestroyed);
	ActorTracker->OnUpdatesWorkerThread.AddUObject(this, &UCrowdyActorManager::HandleUpdateBatch);

	// Deferred activations fire when EntitySubsystem finishes processing a spawn event.
	EntitySubsystem->OnEntityRegistered.AddDynamic(this, &UCrowdyActorManager::OnEntityRegistered);
}

void UCrowdyActorManager::Deinitialize()
{
	if (IsValid(EntitySubsystem))
		EntitySubsystem->OnEntityRegistered.RemoveDynamic(this, &UCrowdyActorManager::OnEntityRegistered);

	UCrowdyActorTracker* CrowdyActorTracker = GetWorld()->GetSubsystem<UCrowdyActorTracker>();
	if (IsValid(CrowdyActorTracker))
	{
		CrowdyActorTracker->OnRemoteEntityAppeared.RemoveAll(this);
		CrowdyActorTracker->OnRemoteEntityTimedOut.RemoveAll(this);
		CrowdyActorTracker->OnUpdatesWorkerThread.RemoveAll(this);
	}

	FCrowdyActorUpdate Discarded;
	while (UpdateQueue.Dequeue(Discarded)) {}

	Slots.Empty();
	UUIDToSlot.Empty();
	PendingActivations.Empty();
	EntitySubsystem = nullptr;

	if (IsValid(ActiveBackend))
	{
		ActiveBackend->DeinitializeBackend();
		ActiveBackend = nullptr;
	}

	Super::Deinitialize();
}

bool UCrowdyActorManager::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
		return false;

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World)
		return false;

	if (World->WorldType != EWorldType::PIE &&
		World->WorldType != EWorldType::Game)
		return false;

	return true;
}

void UCrowdyActorManager::Tick(float DeltaTime)
{
	ApplyPendingUpdates();
	TickInterpolation();
}

void UCrowdyActorManager::RegisterStateClass(UScriptStruct* Struct, TSubclassOf<AActor> ActorClass)
{
	if (Struct && ActorClass)
		StateClassMap.Add(Struct, ActorClass);
}

void UCrowdyActorManager::SetBackend(UCrowdyRenderingBackend* NewBackend)
{
	if (!IsValid(NewBackend))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy Actor Manager]: Invalid backend provided"));
		return;
	}

	if (NewBackend == ActiveBackend)
		return;

	if (IsValid(ActiveBackend))
		ActiveBackend->DeinitializeBackend();

	ActiveBackend = NewBackend;
}

void UCrowdyActorManager::UpdateServerTimeOffset(const int64 ServerTimestampMs)
{
	const int64 ClientNowMs = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds();
	const int64 EstimatedOffset = ServerTimestampMs - ClientNowMs;

	if (!bHasInitialOffset)
	{
		ServerTimeOffsetMs = EstimatedOffset;
		BestOffsetMs = EstimatedOffset;
		bHasInitialOffset = true;
		return;
	}

	if (EstimatedOffset > BestOffsetMs)
		BestOffsetMs = EstimatedOffset;

	if (FMath::Abs(EstimatedOffset - ServerTimeOffsetMs) > 200)
		return;

	ServerTimeOffsetMs = static_cast<int64>(FMath::Lerp(
		static_cast<double>(ServerTimeOffsetMs),
		static_cast<double>(BestOffsetMs),
		0.05
	));
}

int64 UCrowdyActorManager::GetEstimatedServerTimeMs() const
{
	const int64 ClientNowMs = (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds();
	return ClientNowMs + ServerTimeOffsetMs;
}

void UCrowdyActorManager::ApplyPendingUpdates()
{
	if (UpdateQueue.IsEmpty() || !IsValid(ActiveBackend)) return;

	FCrowdyActorUpdate Update;
	while (UpdateQueue.Dequeue(Update))
	{
		UpdateServerTimeOffset(Update.ServerTimestamp);

		const int32* SlotPtr = UUIDToSlot.Find(Update.UUID);
		if (!SlotPtr) continue;

		ActiveBackend->ExtractUpdate(Update.State, Update.ServerTimestamp, *SlotPtr);
	}
}

void UCrowdyActorManager::TickInterpolation()
{
	if (!IsValid(ActiveBackend)) return;

	const int64 RenderTime = GetEstimatedServerTimeMs() - InterpolationDelayMs;
	const int32 Count = Slots.Num();

	for (int32 i = 0; i < Count; i++)
	{
		if (!Slots[i].bActive) continue;
		ActiveBackend->ApplyInterpolation(i, RenderTime);
	}
}

int32 UCrowdyActorManager::AllocateSlot(const FGuid& UUID)
{
	for (int32 i = 0; i < Slots.Num(); i++)
	{
		if (!Slots[i].bActive)
		{
			Slots[i].bActive = true;
			Slots[i].UUID    = UUID;
			UUIDToSlot.Add(UUID, i);
			return i;
		}
	}

	FSlotEntry NewSlot;
	NewSlot.bActive = true;
	NewSlot.UUID    = UUID;

	const int32 SlotId = Slots.Add(NewSlot);
	UUIDToSlot.Add(UUID, SlotId);
	return SlotId;
}

void UCrowdyActorManager::FreeSlot(const FGuid& UUID)
{
	const int32* SlotPtr = UUIDToSlot.Find(UUID);
	if (!SlotPtr) return;

	const int32 SlotId = *SlotPtr;

	if (Slots.IsValidIndex(SlotId))
	{
		Slots[SlotId].bActive = false;
		Slots[SlotId].UUID    = FGuid{};
	}

	UUIDToSlot.Remove(UUID);
}

bool UCrowdyActorManager::LoadConfig()
{
	const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(GetWorld());
	if (!Profile)
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyActorManager]: No map profile configured for this map."));
		return false;
	}

	const FCrowdyActorManagementConfigStruct& Config = Profile->ActorManagement;

	if (!Config.bUseCrowdyActorTracker)
		return false;

	if (!IsValid(Config.BackendClass.Get()))
	{
		UE_LOG(LogCrowdyReplication, Warning, TEXT("[CrowdyActorManager]: BackendClass is not set in the map profile. Assign a UCrowdyRenderingBackend subclass."));
		return false;
	}

	UCrowdyRenderingBackend* Backend = NewObject<UCrowdyRenderingBackend>(this, Config.BackendClass.Get());
	Backend->InitializeBackend(GetWorld(), Config.BackendConfig);
	SetBackend(Backend);

	return true;
}

UClass* UCrowdyActorManager::ResolveEntityClass(const FGuid& UUID, const FInstancedStruct& State) const
{
	// Primary path for dynamic entities: derive class from the state struct type.
	// This works without a spawn event — the position update carries the struct type.
	if (const UScriptStruct* StateStruct = State.GetScriptStruct())
	{
		if (const TSubclassOf<AActor>* Found = StateClassMap.Find(StateStruct))
			return Found->Get();
	}

	// Fallback: entity was registered via a spawn event (static entities, or dynamic
	// entities where the spawn event arrived before position updates).
	if (IsValid(EntitySubsystem))
	{
		const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(UUID);
		if (!Record) return nullptr;

		const FSoftClassPath ClassPath = UCrowdyClassRegistry::Get()->Resolve(Record->ClassID);
		if (!ClassPath.IsValid()) return nullptr;

		return ClassPath.ResolveClass();
	}

	return nullptr;
}

void UCrowdyActorManager::HandleActorSpawned(FGuid UUID, FInstancedStruct InitialState, int32 ActorCount)
{
	if (!IsValid(ActiveBackend)) return;

	// Slot already exists: duplicate tracker broadcast or cascade from a pool actor's own
	// auto-replication. Ignore — the entity is already active.
	if (UUIDToSlot.Contains(UUID)) return;

	UClass* EntityClass = ResolveEntityClass(UUID, InitialState);

	if (!EntityClass)
	{
		// Class not resolvable yet — defer until a spawn event registers the entity.
		FPendingActivation& Pending = PendingActivations.Add(UUID);
		Pending.SlotId       = AllocateSlot(UUID);
		Pending.InitialState = MoveTemp(InitialState);
		return;
	}

	const int32 SlotId = AllocateSlot(UUID);
	ActiveBackend->ActivateInstance(SlotId, UUID, EntityClass, InitialState);
	ActiveBackend->ExtractUpdate(InitialState, GetEstimatedServerTimeMs(), SlotId);

}

void UCrowdyActorManager::HandleActorDestroyed(FGuid UUID, int32 ActorCount)
{
	// Drop any pending activation that never fired.
	PendingActivations.Remove(UUID);

	const int32* SlotPtr = UUIDToSlot.Find(UUID);
	if (!SlotPtr) return;

	if (IsValid(ActiveBackend))
		ActiveBackend->DeactivateInstance(*SlotPtr, UUID);

	FreeSlot(UUID);
}

void UCrowdyActorManager::HandleUpdateBatch(const TArray<FCrowdyActorUpdate>& Updates)
{
	for (const FCrowdyActorUpdate& Update : Updates)
		UpdateQueue.Enqueue(Update);
}

void UCrowdyActorManager::OnEntityRegistered(const FGuid& EntityID)
{
	FPendingActivation Pending;
	if (!PendingActivations.RemoveAndCopyValue(EntityID, Pending))
		return;

	UClass* EntityClass = ResolveEntityClass(EntityID, Pending.InitialState);
	if (!EntityClass)
	{
		// Class still not resolvable after the spawn event — silently drop.
		// This can happen if the class path in the spawn event was invalid.
		FreeSlot(EntityID);
		return;
	}

	if (!IsValid(ActiveBackend))
	{
		FreeSlot(EntityID);
		return;
	}

	ActiveBackend->ActivateInstance(Pending.SlotId, EntityID, EntityClass, Pending.InitialState);
	ActiveBackend->ExtractUpdate(Pending.InitialState, GetEstimatedServerTimeMs(), Pending.SlotId);
}
