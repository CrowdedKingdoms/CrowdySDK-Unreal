// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Subsystems/CrowdyActorManager.h"

#include "Data/CrowdyRepApplicationPolicy.h"
#include "Replication/Subsystems/CrowdyActorPoolSubsystem.h"
#include "Replication/Subsystems/CrowdyActorTracker.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

void UCrowdyActorManager::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	const UWorld* World = GetWorld();
	
	check(IsValid(World))
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[Crowdy Actor Manager]: Invalid world"));
		return;
	}
	
	ActorTracker = World->GetSubsystem<UCrowdyActorTracker>();
	ActorPool = World->GetSubsystem<UCrowdyActorPoolSubsystem>();
	
	check(IsValid(ActorTracker))
	check(IsValid(ActorPool))
	
	if (!IsValid(ActorTracker))
	{
		UE_LOG(LogTemp, Error, TEXT("[Crowdy Actor Manager]: Invalid actor tracker"));
		return;
	}
	
	if (!IsValid(ActorPool))
	{
		UE_LOG(LogTemp, Error, TEXT("[Crowdy Actor Manager]: Invalid actor pool"));
		return;
	}
	
	if (!LoadConfig())
	{
		UE_LOG(LogTemp, Error, TEXT("[Crowdy Actor Manager]: Failed to load config due to incorrect params or disabled by choice."));
		return;
	}
	
	ActorTracker->OnNewPlayerJoined.AddDynamic(this, &UCrowdyActorManager::HandleActorSpawned);
	ActorTracker->OnPlayerLeft.AddDynamic(this, &UCrowdyActorManager::HandleActorDestroyed);
	ActorTracker->OnUpdatesWorkerThread.AddUObject(this, &UCrowdyActorManager::HandleUpdateBatch);
	
}

void UCrowdyActorManager::Deinitialize()
{
	// Detach from tracker delegates
	UCrowdyActorTracker* CrowdyActorTracker = GetWorld()->GetSubsystem<UCrowdyActorTracker>();
	if (IsValid(CrowdyActorTracker))
	{
		CrowdyActorTracker->OnNewPlayerJoined.RemoveAll(this);
		CrowdyActorTracker->OnPlayerLeft.RemoveAll(this);
		CrowdyActorTracker->OnUpdatesWorkerThread.RemoveAll(this);
	}

	// Drain the update queue
	FCrowdyActorUpdate Discarded;
	while (UpdateQueue.Dequeue(Discarded)) {}

	// Clear slots
	Slots.Empty();
	UUIDToSlot.Empty();
	PendingRemovals.Empty();

	// Clear policy
	if (IsValid(ActivePolicy))
		ActivePolicy = nullptr;

	ReplicatedActorClass = nullptr;

	Super::Deinitialize();
}

bool UCrowdyActorManager::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	const UWorld* World = Outer ? Outer->GetWorld() : nullptr;
	if (!World)
	{
		return false;
	}

	// PIE or Game only
	if (World->WorldType != EWorldType::PIE &&
		World->WorldType != EWorldType::Game)
	{
		return false;
	}
	
	return true;
}

void UCrowdyActorManager::Tick(float DeltaTime)
{
	ApplyPendingUpdates();
	TickInterpolation();
	ProcessRemovals();
}

void UCrowdyActorManager::SetPolicy(UCrowdyRepApplicationPolicy* NewPolicy)
{
	if (!IsValid(NewPolicy))
	{
		UE_LOG(LogTemp, Error, TEXT("[Crowdy Actor Manager]: Invalid policy provided to actor manager"));
		return;
	}
	
	if (NewPolicy == ActivePolicy)
	{
		UE_LOG(LogTemp, Error, TEXT("[Crowdy Actor Manager]: Policy already set"));
		return;
	}
	
	ActivePolicy = NewPolicy;
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
	if (UpdateQueue.IsEmpty()) return;
	
	FCrowdyActorUpdate Update;
	while (UpdateQueue.Dequeue(Update))
	{
		UpdateServerTimeOffset(Update.ServerTimestamp);
		const int32* SlotPtr = UUIDToSlot.Find(Update.UUID);
		
		if (!SlotPtr) continue;
		
		ActivePolicy->ExtractFields(Update.State, Update.ServerTimestamp, *SlotPtr);
	}
}

void UCrowdyActorManager::TickInterpolation()
{
	const int64 RenderTime = GetEstimatedServerTimeMs() - InterpolationDelayMs;
	const int32 Count = Slots.Num();
	
	for (int32 i = 0; i < Count; i++)
	{
		if (!Slots[i].bActive) continue;
		
		AActor* Actor = Slots[i].Actor.Get();
		if (!IsValid(Actor)) continue;
		ActivePolicy->ApplyToActor(Actor, i, RenderTime);
	}
}

void UCrowdyActorManager::ProcessRemovals()
{
	if (PendingRemovals.IsEmpty()) return;
	
	for (const int32 SlotId : PendingRemovals)
	{
		if (Slots.IsValidIndex(SlotId))
			continue;
		
		if (ActivePolicy)
			ActivePolicy->OnInstanceDeactivated(SlotId);
	}
	
	PendingRemovals.Empty();
}

int32 UCrowdyActorManager::AllocateSlot(const FGuid& UUID, AActor* Actor)
{
	// Reuse a free slot if available
	for (int32 i = 0; i < Slots.Num(); i++)
	{
		if (!Slots[i].bActive)
		{
			Slots[i].bActive = true;
			Slots[i].UUID    = UUID;
			Slots[i].Actor   = Actor;
			UUIDToSlot.Add(UUID, i);
			return i;
		}
	}

	// No free slot — append a new one
	FSlotEntry NewSlot;
	NewSlot.bActive = true;
	NewSlot.UUID    = UUID;
	NewSlot.Actor   = Actor;

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
		Slots[SlotId].Actor   = nullptr;
	}

	UUIDToSlot.Remove(UUID);
	PendingRemovals.Add(SlotId);
}

bool UCrowdyActorManager::LoadConfig()
{
	const UCrowdySDKDeveloperSettings* DeveloperSettings = GetDefault<UCrowdySDKDeveloperSettings>();
	if (!DeveloperSettings)
		return false;

	const UWorld* World = GetWorld();
	if (!IsValid(World))
		return false;

	FString CurrentMap = FPackageName::GetShortName(World->GetOutermost()->GetName());

#if WITH_EDITOR
	if (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE)
		CurrentMap = World->RemovePIEPrefix(CurrentMap);
#endif

	for (const auto& [WorldPtr, ConfigAssetPtr] : DeveloperSettings->ActorManagementConfigs)
	{
		if (WorldPtr.IsNull())
			continue;

		const FString AllowedMap = FPackageName::GetShortName(WorldPtr.GetAssetName());
		if (AllowedMap != CurrentMap)
			continue;

		const UCrowdyActorManagementConfig* ConfigAsset = ConfigAssetPtr.LoadSynchronous();
		if (!IsValid(ConfigAsset))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CrowdyActorManager]: Config asset for map '%s' is invalid."), *CurrentMap);
			return false;
		}

		const FCrowdyActorManagementConfigStruct& Config = ConfigAsset->Config;

		if (!Config.bUseCrowdyActorTracker)
			return false;

		// Register pool
		ActorPool->RegisterPool(Config.ActorPoolConfig);
		ReplicatedActorClass = Config.ActorPoolConfig.ActorClass.Get();

		// Validate and apply replication policy
		if (!IsValid(Config.ReplicationPolicyClass.Get()))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CrowdyActorManager]: ReplicationPolicyClass is invalid for map '%s'."), *CurrentMap);
			return false;
		}

		UCrowdyRepApplicationPolicy* Policy = NewObject<UCrowdyRepApplicationPolicy>(this, Config.ReplicationPolicyClass.Get());
		SetPolicy(Policy);

		return true;
	}

	UE_LOG(LogTemp, Warning, TEXT("[CrowdyActorManager]: Map '%s' is not in the allowed levels list."), *CurrentMap);
	return false;
}

void UCrowdyActorManager::HandleActorSpawned(FGuid UUID, FInstancedStruct InitialState, int32 ActorCount)
{
	// Release first if already tracked — handles stop/restart for same UUID
	if (ActorPool->FindActor(UUID))
		ActorPool->ReleaseActor(UUID);

	AActor* Actor = ActorPool->AcquireActor(ReplicatedActorClass, UUID, InitialState);
	if (!Actor) return;

	const int32 SlotId = AllocateSlot(UUID, Actor);
	if (ActivePolicy)
		ActivePolicy->ExtractFields(InitialState, GetEstimatedServerTimeMs(), SlotId);
}

void UCrowdyActorManager::HandleActorDestroyed(FGuid UUID, int32 ActorCount)
{
	ActorPool->ReleaseActor(UUID);
	FreeSlot(UUID);
}

void UCrowdyActorManager::HandleUpdateBatch(const TArray<FCrowdyActorUpdate>& Updates)
{
	for (const FCrowdyActorUpdate& Update : Updates)
	{
		UpdateQueue.Enqueue(Update);
	}
}
