// Fill out your copyright notice in the Description page of Project Settings.


#include "Replication/Subsystems/CrowdyAutoReplicator.h"

#include "Messages/Actor/FActorUpdateRequestMessage.h"
#include "Replication/Components/CrowdyActorUpdateComponent.h"
#include "Subsystem/CrowdySDKSubsystem.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Utils/HelperFunctions.h"

void UCrowdyAutoReplicator::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	
	const UWorld* World = GetWorld();
	
	if (!IsValid(World))
		return;
	
	CrowdySDK = World->GetGameInstance()->GetSubsystem<UCrowdySDKSubsystem>();
	check(CrowdySDK);

	
	if (!IsValid(CrowdySDK))
	{
		UE_LOG(LogTemp, Error, TEXT("[Crowdy SDK][AutoReplicator]: Invalid Crowdy SDK subsystem."));
		return;
	}

	constexpr int32 ExpectedActor = 2048;
	Data.Components.Reserve(ExpectedActor);
	Data.Positions.Reserve(ExpectedActor);
	Data.Chunks.Reserve(ExpectedActor);
	Data.UUIDs.Reserve(ExpectedActor);
	
	const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>();
	
	if (!Settings || !Settings->bUseAutoReplicator)
	{
		return;
	}

	FString CurrentMap = FPackageName::GetShortName(World->GetOutermost()->GetName());
	
#if WITH_EDITOR
	if (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE)
	{
		FString Prefix;
		CurrentMap = World->RemovePIEPrefix(CurrentMap);
	}
#endif
	
	//UE_LOG(LogTemp, Log, TEXT("[Crowdy SDK][AutoReplicator]: Current map: %s"), *CurrentMap);
	
	bool bAllowed = false;
	
	for (const TSoftObjectPtr<UWorld>& WorldPtr : Settings->LevelsToUseAutoReplicator)
	{
		if (WorldPtr.IsNull())
		{
			continue;
		}

		const FString AllowedMap = FPackageName::GetShortName(WorldPtr.GetAssetName());
		//UE_LOG(LogTemp, Log, TEXT("[Crowdy SDK][AutoReplicator]: Allowed map: %s"), *AllowedMap);

		if (AllowedMap == CurrentMap)
		{
			bAllowed = true;
			break;
		}
	}
	
	//UE_LOG(LogTemp, Log, TEXT("[Crowdy SDK][AutoReplicator]: AutoReplicator is %s"), bAllowed ? TEXT("enabled") : TEXT("disabled"));
	
	bIsTicking = bAllowed;
	
	ReplicationInterval = static_cast<float>(Settings->ReplicationIntervalHz)/100.0f;
	//UE_LOG(LogTemp, Log, TEXT("[Crowdy SDK][AutoReplicator]: Replication interval: %f"), ReplicationInterval);
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

void UCrowdyAutoReplicator::RegisterReplicationComponent(UCrowdyActorUpdateComponent* Component)
{
	if (!IsValid(Component)) return;

	// Write UUID/State by value — no dangling pointer risk
	Data.Components.Add(Component);
	Data.Positions.Add(FVector3f::ZeroVector);
	Data.Chunks.Add(FInt64Vector::ZeroValue);
	Data.UUIDs.Add(Component->GetUUID());           // copy
	Data.Count++;
}

void UCrowdyAutoReplicator::UnregisterReplicationComponent(UCrowdyActorUpdateComponent* Component)
{
	if (!IsValid(Component)) return;

	// Swap-remove to avoid O(n) shifts
	for (int32 i = 0; i < Data.Count; i++)
	{
		if (Data.Components[i] != Component) continue;

		const int32 Last = Data.Count - 1;
		Data.Components.RemoveAtSwap(i);
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
		const UCrowdyActorUpdateComponent* Comp = Data.Components[i].Get();
		
		if (!Comp) 
			continue;

		const FVector3f NewPos = FVector3f(Comp->GetCachedOwner()->GetActorLocation());
		Data.Positions[i]    = NewPos;
		UHelperFunctions::GetChunkCoordinateAtLocation(this,FVector(NewPos), Data.Chunks[i].X, Data.Chunks[i].Y, Data.Chunks[i].Z);
		
		
		CrowdySDK->DispatchActorUpdate(Data.Chunks[i].X, Data.Chunks[i].Y, Data.Chunks[i].Z, 
			ECrowdyDecayRate::No_Decay, 
			ECrowdyReplicationDistance::Eight_Chunks,
			Data.UUIDs[i], 
			Comp->GetActorState(), 
			true);
	}
	
}
