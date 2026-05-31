// Fill out your copyright notice in the Description page of Project Settings.


#include "Utils/CrowdyUtilities.h"

#include "Replication/Components/CrowdyActorUpdateComponent.h"
#include "Replication/Interfaces/CrowdyActorUpdateComponentInterface.h"
#include "Replication/Subsystems/CrowdyActorPoolSubsystem.h"
#include "Replication/Subsystems/CrowdyEntityManager.h"
#include "Replication/Subsystems/CrowdyEventManager.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystem/CrowdyGameSession.h"
#include "Subsystem/CrowdyHostSubsystem.h"
#include "Subsystem/CrowdySDKSubsystem.h"

AActor* UCrowdyUtilities::SpawnCrowdyEntity(UObject* WorldContextObject, TSubclassOf<AActor> EntityActorClass,
                                            const FTransform& SpawnTransform, const FInstancedStruct& InitialState)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return nullptr;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return nullptr;
	}
	
	if (!IsValid(EntityActorClass))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: EntityClass is null."));
		return nullptr;
	}
	
	if (!InitialState.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CrowdyUtilities]: InitialState is null. State is optional to send."));
	}

	const TObjectPtr<UCrowdyEntityManager> EntityManager = World->GetSubsystem<UCrowdyEntityManager>();
	
	if (!IsValid(EntityManager.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: EntityManager is null."));
		return nullptr;
	}
	
	return EntityManager->SpawnCrowdyEntity(EntityActorClass, SpawnTransform, InitialState);
}

void UCrowdyUtilities::UpdateCrowdyEntityState(UObject* WorldContextObject, AActor* TargetEntity,
	const FInstancedStruct& NewState)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return;
	}
	
	if (!IsValid(TargetEntity))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: TargetEntity is null."));
		return;
	}
	
	if (!NewState.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: NewState is null."));
		return;
	}
	
	const TObjectPtr<UCrowdyEntityManager> EntityManager = World->GetSubsystem<UCrowdyEntityManager>();
	
	if (!IsValid(EntityManager.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: EntityManager is null."));
		 return;
	}
	
	EntityManager->UpdateCrowdyEntityState(TargetEntity, NewState);
}

void UCrowdyUtilities::DestroyCrowdyEntity(UObject* WorldContextObject, AActor* TargetEntity)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return;
	}
	
	const TObjectPtr<UCrowdyEntityManager> EntityManager = World->GetSubsystem<UCrowdyEntityManager>();
	
	if (!IsValid(EntityManager.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: EntityManager is null."));
		return;
	}
	
	EntityManager->DestroyCrowdyEntity(TargetEntity);
}

void UCrowdyUtilities::RegisterStaticEntity(UObject* WorldContextObject, AActor* Entity, const bool bUseDeterministicID,
	const int64 Seed)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return;
	}
	
	if (!IsValid(Entity))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: Entity is null."));
		return;
	}
	
	const TObjectPtr<UCrowdyEntityManager> EntityManager = World->GetSubsystem<UCrowdyEntityManager>();
	
	if (!IsValid(EntityManager.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: EntityManager is null."));
		return;
	}
	
	EntityManager->RegisterStaticEntity(Entity, bUseDeterministicID, Seed);
}

AActor* UCrowdyUtilities::FindCrowdyEntity(UObject* WorldContextObject, bool& bIsValid, const FGuid& EntityID)
{
	bIsValid = false;
	
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return nullptr;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return nullptr;
	}
	
	const TObjectPtr<UCrowdyEntityManager> EntityManager = World->GetSubsystem<UCrowdyEntityManager>();
	
	if (!IsValid(EntityManager.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: EntityManager is null."));
		return nullptr;
	}
	
	AActor* Entity = EntityManager->FindEntity(EntityID);
	
	if (IsValid(Entity))
	{
		bIsValid = true;
		return Entity;
	}
	
	return nullptr;
	
}

FGuid UCrowdyUtilities::FindCrowdyEntityID(UObject* WorldContextObject, bool& bIsValid, const AActor* Entity)
{
	bIsValid = false;
	
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return FGuid();
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return FGuid();
	}
	
	const TObjectPtr<UCrowdyEntityManager> EntityManager = World->GetSubsystem<UCrowdyEntityManager>();
	
	if (!IsValid(EntityManager.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: EntityManager is null."));
		 return FGuid();
	}

	const FGuid EntityID = EntityManager->FindEntityID(Entity);
	
	if (EntityID.IsValid())
	{
		bIsValid = true;
		return EntityID;
	}
	
	return FGuid();
}

bool UCrowdyUtilities::IsCrowdyEntityLocallyOwned(UObject* WorldContextObject, const AActor* Entity)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return false;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return false;
	}
	const TObjectPtr<UCrowdyEntityManager> EntityManager = World->GetSubsystem<UCrowdyEntityManager>();
	
	if (!IsValid(EntityManager.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: EntityManager is null."));
		return false;
	}
	
	return EntityManager->IsLocallyOwned(Entity);
}

FGuid UCrowdyUtilities::FindCrowdyActorID(UObject* WorldContextObject, const AActor* Actor, bool& bIsValid)
{
	bIsValid = false;

	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return {};
	}

	const UWorld* World = WorldContextObject->GetWorld();

	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return {};
	}

	if (!IsValid(Actor))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: Input Actor is null."));
		return {};
	}

	auto ReturnIfValid =
		[&bIsValid, Actor](const FGuid& ID, const TCHAR* Source) -> TOptional<FGuid>
	{
		if (!ID.IsValid())
		{
			return {};
		}

		bIsValid = true;
			
		/*	
		UE_LOG(
			LogTemp,
			Log,
			TEXT("[CrowdyUtilities]: Found ID %s for Actor [%s] via %s."),
			*ID.ToString(),
			*GetNameSafe(Actor),
			Source);
		*/
		return ID;
	};

	// Interface path
	if (const ICrowdyActorUpdateComponentInterface* Interface =
		Cast<const ICrowdyActorUpdateComponentInterface>(Actor))
	{
		if (const TOptional<FGuid> Result =
			ReturnIfValid(
				Interface->GetCrowdyActorUpdateComponent()->GetID(),
				TEXT("ICrowdyActorUpdateComponentInterface")))
		{
			return *Result;
		}
	}

	// Component path
	if (const UCrowdyActorUpdateComponent* Component =
		Actor->FindComponentByClass<UCrowdyActorUpdateComponent>())
	{
		if (const TOptional<FGuid> Result =
			ReturnIfValid(
				Component->GetID(),
				TEXT("UCrowdyActorUpdateComponent")))
		{
			return *Result;
		}
	}

	const TObjectPtr<UCrowdyActorPoolSubsystem> ActorPool = World->GetSubsystem<UCrowdyActorPoolSubsystem>();

	if (const TOptional<FGuid> Result =
		ReturnIfValid(
			ActorPool->FindActorID(Actor),
			TEXT("UCrowdyActorPoolSubsystem")))
	{
		return *Result;
	}

	const TObjectPtr<UCrowdyEntityManager> EntityManager = World->GetSubsystem<UCrowdyEntityManager>();

	if (const TOptional<FGuid> Result =
		ReturnIfValid(
			EntityManager->FindEntityID(Actor),
			TEXT("UCrowdyEntityManager")))
	{
		return *Result;
	}

	UE_LOG(LogTemp, Warning, TEXT("[CrowdyUtilities]: Failed to find ID for Actor [%s]."), *GetNameSafe(Actor));

	return {};
}

AActor* UCrowdyUtilities::FindCrowdyActor(UObject* WorldContextObject, const FGuid& ID, bool& bIsValid)
{
	bIsValid = false;
	
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return nullptr;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return nullptr;
	}
	
	const TObjectPtr<UCrowdyActorPoolSubsystem> ActorPool = World->GetSubsystem<UCrowdyActorPoolSubsystem>();
	
	if (IsValid(ActorPool.Get()))
	{
		
		AActor* TargetActor =  ActorPool->FindActor(ID);
		if (IsValid(TargetActor))
		{
			bIsValid = true;
			return TargetActor;
		}
	}
	
	const TObjectPtr<UCrowdyEntityManager> EntityManager = World->GetSubsystem<UCrowdyEntityManager>();
	
	if (IsValid(EntityManager.Get()))
	{
		AActor* TargetActor = EntityManager->FindEntity(ID);
		if (IsValid(TargetActor))
		{
			bIsValid = true;
			return TargetActor;
		}
	}
	
	//UE_LOG(LogTemp, Warning, TEXT("[CrowdyUtilities]: Failed to find Actor for ID [%s]."), *ID.ToString());
	return nullptr;
}

void UCrowdyUtilities::CrowdyHasAuthority(UObject* WorldContextObject, bool& bHasAuthority)
{
	bHasAuthority = CrowdyHasAuthority(WorldContextObject);
}

void UCrowdyUtilities::IsCrowdyActorPlayerControlled(UObject* WorldContextObject, AActor* TargetActor,
	bool& bIsPlayerControlled)
{
	bIsPlayerControlled = false;
	
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return;
	}
	
	if (!IsValid(TargetActor))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: TargetActor is null."));
	}
	
	if (const ICrowdyActorUpdateComponentInterface* Interface = Cast<const ICrowdyActorUpdateComponentInterface>(TargetActor))
	{
		bIsPlayerControlled = true;
		return;
	}
	
	const TObjectPtr<UCrowdyActorPoolSubsystem> ActorPool = World->GetSubsystem<UCrowdyActorPoolSubsystem>();
	
	if (!IsValid(ActorPool.Get()))
	{
		bIsPlayerControlled = false;
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: ActorPool is null."));
		return;
	}

	const FGuid ActorID = ActorPool->FindActorID(TargetActor);
	
	if (ActorID.IsValid())
	{
		bIsPlayerControlled = true;
		return;
	}
}

bool UCrowdyUtilities::CrowdyHasAuthority(const UObject* WorldContextObject)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return false;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return false;
	}

	const TObjectPtr<UCrowdyHostSubsystem> HostSubsystem = World->GetSubsystem<UCrowdyHostSubsystem>();
	
	if (!IsValid(HostSubsystem.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: GameSession is null."));
		return false;
	}
	
	return HostSubsystem->IsHost();
}

void UCrowdyUtilities::RegisterForEventReception(UObject* WorldContextObject, UObject* Object)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return;
	}

	const TObjectPtr<UCrowdyEventManager> EventManager = World->GetSubsystem<UCrowdyEventManager>();
	
	if (!IsValid(EventManager.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: EventManager is null."));
		return;
	}
	
	if (!IsValid(Object))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: Object is null."));
		return;
	}	
	
	EventManager->RegisterHandler(Object);
}

void UCrowdyUtilities::UnregisterForEventReception(UObject* WorldContextObject, UObject* Object)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return;
	}

	const TObjectPtr<UCrowdyEventManager> EventManager = World->GetSubsystem<UCrowdyEventManager>();
	
	if (!IsValid(EventManager.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: EventManager is null."));
		return;
	}
	
	if (!IsValid(Object))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: Object is null."));
		return;
	}	
	
	EventManager->UnregisterHandler(Object);
}


void UCrowdyUtilities::DispatchGameEvent(const UObject* WorldContextObject, int64 ChunkX, int64 ChunkY, int64 ChunkZ,
	ECrowdyDecayRate DecayRate, ECrowdyReplicationDistance ReplicationDistance, const FGuid& InstigatorID,
	FInstancedStruct EventPayload, bool bAsync)
{
	if (!IsValid(WorldContextObject))
	{
		return;
	}

	const UWorld* World = WorldContextObject->GetWorld();
	if (!World)
	{
		return;
	}

	UGameInstance* GI = World->GetGameInstance();
	if (!GI)
	{
		return;
	}

	UCrowdySDKSubsystem* SDK =
		GI->GetSubsystem<UCrowdySDKSubsystem>();

	if (!IsValid(SDK))
	{
		UE_LOG(LogTemp, Error,
			TEXT("[CrowdySDK]: SDK subsystem unavailable."));
		return;
	}
	
	SDK->DispatchGameEvent(ChunkX, ChunkY, ChunkZ, DecayRate, ReplicationDistance, InstigatorID, EventPayload, bAsync);
}

bool UCrowdyUtilities::CrowdyIsConnectedToServer(UObject* WorldContextObject)
{
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		return false;
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		 return false;
	}
	
	const TObjectPtr<UCrowdySDKSubsystem> SDK = World->GetGameInstance()->GetSubsystem<UCrowdySDKSubsystem>();
	
	if (!IsValid(SDK.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: SDK is null."));
		return false;
	}
	
	return SDK->GetUDPConnectionState() == EUDPConnectionState::Connected;

}

FGuid UCrowdyUtilities::CrowdyGetHostID(UObject* WorldContextObject, bool& bIsValid)
{
	bIsValid = false;
	
	if (!IsValid(WorldContextObject))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: WorldContextObject is null."));
		 return {};
	}
	
	const UWorld* World = WorldContextObject->GetWorld();
	
	if (!IsValid(World))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: World is null."));
		return {};
	}
	
	const TObjectPtr<UCrowdyHostSubsystem> HostSubsystem = World->GetSubsystem<UCrowdyHostSubsystem>();
	
	if (!IsValid(HostSubsystem.Get()))
	{
		UE_LOG(LogTemp, Error, TEXT("[CrowdyUtilities]: GameSession is null."));
		return {};
	}
	
	if (HostSubsystem->GetHostID().IsValid())
	{
		bIsValid = true;
		 return HostSubsystem->GetHostID();
	}
	
	return {};
}
