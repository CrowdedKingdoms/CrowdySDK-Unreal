// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Subsystems/CrowdyAutoReplicator.h"
#include "CrowdyReplicationLog.h"

#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Replication/Interfaces/CrowdyReplicationSource.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Internal/FCrowdyServiceRegistry.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/HelperFunctions.h"

void UCrowdyAutoReplicator::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	const UWorld* World = GetWorld();

	if (!IsValid(World))
		return;

	UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance)
		return;

	Bridge = GameInstance->GetSubsystem<UCrowdySDKBridgeSubsystem>();
	check(Bridge);


	if (!IsValid(Bridge))
	{
		UE_LOG(LogCrowdyReplication, Error, TEXT("[Crowdy SDK][AutoReplicator]: Invalid Bridge subsystem."));
		return;
	}

	constexpr int32 ExpectedActor = 2048;
	Data.Components.Reserve(ExpectedActor);
	Data.Sources.Reserve(ExpectedActor);
	Data.Positions.Reserve(ExpectedActor);
	Data.Chunks.Reserve(ExpectedActor);
	Data.UUIDs.Reserve(ExpectedActor);
	
	const UCrowdyMapProfile* Profile = UCrowdySDKDeveloperSettings::ResolveProfileForWorld(World);
	bIsTicking = Profile && Profile->bUseAutoReplicator;

	if (bIsTicking)
	{
		ReplicationInterval = 1.0f / FMath::Max(1, Profile->ReplicationIntervalHz);
	}
}

void UCrowdyAutoReplicator::Deinitialize()
{
	Super::Deinitialize();
}

bool UCrowdyAutoReplicator::ShouldCreateSubsystem(UObject* Outer) const
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

TStatId UCrowdyAutoReplicator::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(AutoReplicator, STATGROUP_Tickables);
}

void UCrowdyAutoReplicator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	
	if (!bIsTicking)
		return;
	
	ReplicationAccumulator += DeltaTime;
	
	if (ReplicationAccumulator >= ReplicationInterval)
	{
		ReplicationAccumulator -= ReplicationInterval;
		ReplicationLoop();
	}
}

void UCrowdyAutoReplicator::RegisterReplicationComponent(UActorComponent* Component)
{
	if (!IsValid(Component)) return;

	const ICrowdyReplicationSource* Source = Cast<ICrowdyReplicationSource>(Component);
	if (!Source)
	{
		UE_LOG(LogCrowdyReplication, Error,
			TEXT("[Crowdy SDK][AutoReplicator]: '%s' does not implement ICrowdyReplicationSource."),
			*GetNameSafe(Component));
		return;
	}

	if (Data.Components.Contains(Component)) return;

	// Write UUID/State by value — no dangling pointer risk
	Data.Components.Add(Component);
	Data.Sources.Add(Source);
	Data.Positions.Add(FVector3f::ZeroVector);
	Data.Chunks.Add(FInt64Vector::ZeroValue);
	Data.UUIDs.Add(Source->GetReplicationUUID());   // copy
	Data.Count++;
}

void UCrowdyAutoReplicator::UnregisterReplicationComponent(UActorComponent* Component)
{
	if (!IsValid(Component)) return;

	// Swap-remove to avoid O(n) shifts
	for (int32 i = 0; i < Data.Count; i++)
	{
		if (Data.Components[i] != Component) continue;

		Data.Components.RemoveAtSwap(i);
		Data.Sources.RemoveAtSwap(i);
		Data.Positions.RemoveAtSwap(i);
		Data.Chunks.RemoveAtSwap(i);
		Data.UUIDs.RemoveAtSwap(i);
		Data.Count--;
		break;
	}
}

void UCrowdyAutoReplicator::ReplicationLoop()
{
	const int32 Count = Data.Count;
	if (Count == 0) return;

	for (int32 i = 0; i < Count; i++)
	{
		if (!Data.Components[i].IsValid())
			continue;

		const ICrowdyReplicationSource* Comp = Data.Sources[i];

		const FVector3f NewPos = FVector3f(Comp->GetReplicatedActor()->GetActorLocation());
		Data.Positions[i]    = NewPos;
		UHelperFunctions::GetChunkCoordinateAtLocation(this,FVector(NewPos), Data.Chunks[i].X, Data.Chunks[i].Y, Data.Chunks[i].Z);
		
		
		if (Bridge && Bridge->DispatchActorUpdateFn)
			Bridge->DispatchActorUpdateFn(Data.Chunks[i].X, Data.Chunks[i].Y, Data.Chunks[i].Z,
				ECrowdyDecayRate::No_Decay,
				ECrowdyReplicationDistance::Eight_Chunks,
				Data.UUIDs[i],
				Comp->GetReplicatedState(),
				true);
	}
	
}
