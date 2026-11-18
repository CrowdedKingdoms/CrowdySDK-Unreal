// Fill out your copyright notice in the Description page of Project Settings.

#include "Utils/CrowdyUtilities.h"

#include "CrowdyServicesLog.h"
#include "Engine/GameInstance.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Interfaces/CrowdyEntityComponentProvider.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyEventRouter.h"
#include "Core/CrowdySDKBridgeSubsystem.h"
#include "Network/UDP/CrowdyUDPSubsystem.h"

namespace CrowdyUtils
{
	UWorld* GetWorldChecked(const UObject* WorldContextObject, const TCHAR* Caller)
	{
		if (!IsValid(WorldContextObject))
		{
			UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdyUtilities]: %s — WorldContextObject is null."), Caller);
			return nullptr;
		}

		UWorld* World = WorldContextObject->GetWorld();
		if (!IsValid(World))
			UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdyUtilities]: %s — World is null."), Caller);

		return World;
	}

	ECrowdyTarget MapRecipientToTarget(ECrowdyEventRecipient Recipient)
	{
		switch (Recipient)
		{
		case ECrowdyEventRecipient::OwningClient: return ECrowdyTarget::Owner;
		// This legacy spatial-only send has no channel transport, so a channel Multicast falls back
		// to everyone-in-range here, same as Spatial Multicast.
		case ECrowdyEventRecipient::Multicast:
		case ECrowdyEventRecipient::SpatialMulticast:
		default:                                  return ECrowdyTarget::Entity;
		}
	}
}

// Entity Lifecycle

AActor* UCrowdyUtilities::SpawnCrowdyEntity(UObject* WorldContextObject, TSubclassOf<AActor> EntityActorClass,
	const FTransform& SpawnTransform, const FInstancedStruct& InitialState)
{
	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("SpawnCrowdyEntity"));
	if (!World) return nullptr;

	if (!IsValid(EntityActorClass))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdyUtilities]: SpawnCrowdyEntity — EntityActorClass is null."));
		return nullptr;
	}

	UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!IsValid(EntitySubsystem))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdyUtilities]: SpawnCrowdyEntity — EntitySubsystem is null."));
		return nullptr;
	}

	return EntitySubsystem->SpawnEntity(EntityActorClass, SpawnTransform, InitialState);
}

void UCrowdyUtilities::DestroyCrowdyEntity(UObject* WorldContextObject, AActor* TargetEntity)
{
	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("DestroyCrowdyEntity"));
	if (!World) return;

	UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!IsValid(EntitySubsystem))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdyUtilities]: DestroyCrowdyEntity — EntitySubsystem is null."));
		return;
	}

	EntitySubsystem->DestroyEntity(TargetEntity);
}

// Lookups

FGuid UCrowdyUtilities::GetCrowdyEntityID(UObject* WorldContextObject, const AActor* Actor, bool& bIsValid)
{
	bIsValid = false;

	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("GetCrowdyEntityID"));
	if (!World) return {};

	if (!IsValid(Actor))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdyUtilities]: GetCrowdyEntityID — Actor is null."));
		return {};
	}

	if (const UCrowdyEntityComponent* Component = Actor->FindComponentByClass<UCrowdyEntityComponent>())
	{
		if (Component->GetNetID().IsValid())
		{
			bIsValid = true;
			return Component->GetNetID();
		}
	}

	if (UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>())
	{
		const FGuid EntityID = EntitySubsystem->FindEntityID(Actor);
		if (EntityID.IsValid())
		{
			bIsValid = true;
			return EntityID;
		}
	}

	return {};
}

AActor* UCrowdyUtilities::GetCrowdyEntity(UObject* WorldContextObject, const FGuid& ID, bool& bIsValid)
{
	bIsValid = false;

	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("GetCrowdyEntity"));
	if (!World) return nullptr;

	UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!IsValid(EntitySubsystem)) return nullptr;

	AActor* Entity = EntitySubsystem->FindEntity(ID);
	if (IsValid(Entity))
	{
		bIsValid = true;
		return Entity;
	}

	return nullptr;
}

FGuid UCrowdyUtilities::GetLocalPlayerEntityID(UObject* WorldContextObject)
{
	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("GetLocalPlayerEntityID"));
	if (!World) return {};

	const UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	return IsValid(EntitySubsystem) ? EntitySubsystem->GetLocalPlayerID() : FGuid{};
}

FGuid UCrowdyUtilities::GetCrowdyEntityWriterID(UObject* WorldContextObject, const AActor* Entity, bool& bIsValid)
{
	bIsValid = false;

	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("GetCrowdyEntityWriterID"));
	if (!World || !IsValid(Entity)) return {};

	const UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!IsValid(EntitySubsystem)) return {};

	const FGuid EntityID = EntitySubsystem->FindEntityID(Entity);
	const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(EntityID);
	if (!Record) return {};

	bIsValid = Record->WriterID.IsValid();
	return Record->WriterID;
}

int64 UCrowdyUtilities::GetCrowdyEntityOwnerUserID(UObject* WorldContextObject, const AActor* Entity)
{
	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("GetCrowdyEntityOwnerUserID"));
	if (!World || !IsValid(Entity)) return 0;

	const UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!IsValid(EntitySubsystem)) return 0;

	const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(EntitySubsystem->FindEntityID(Entity));
	return Record ? EntitySubsystem->GetOwnerUserID(*Record) : 0;
}

TArray<AActor*> UCrowdyUtilities::GetAllCrowdyEntitiesByOwner(UObject* WorldContextObject, const FGuid& OwnerID)
{
	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("GetAllEntitiesByOwner"));
	if (!World) return {};

	const UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	return IsValid(EntitySubsystem) ? EntitySubsystem->GetEntitiesByOwner(OwnerID) : TArray<AActor*>{};
}

bool UCrowdyUtilities::CrowdyIsEntityRegistered(UObject* WorldContextObject, const FGuid& EntityID)
{
	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("CrowdyIsEntityRegistered"));
	if (!World) return false;

	const UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	return IsValid(EntitySubsystem) && EntitySubsystem->FindRecord(EntityID) != nullptr;
}

ECrowdyRole UCrowdyUtilities::GetCrowdyEntityRole(UObject* WorldContextObject, const AActor* Entity)
{
	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("GetCrowdyEntityRole"));
	if (!World || !IsValid(Entity)) return ECrowdyRole::None;

	const UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!IsValid(EntitySubsystem)) return ECrowdyRole::None;

	const FGuid EntityID = EntitySubsystem->FindEntityID(Entity);
	const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(EntityID);
	return Record ? Record->Role : ECrowdyRole::None;
}

bool UCrowdyUtilities::DoesCrowdyEntityOwn(UObject* WorldContextObject, const AActor* OwnerActor, const AActor* TargetActor)
{
	// OwnerActor's NetID is what gets stamped as WriterID on the entities it owns (a player avatar's NetID is its
	// player id), and both ids are network-stable, so the answer is the same on every client.
	bool bOwnerValid = false;
	const FGuid OwnerNetID = GetCrowdyEntityID(WorldContextObject, OwnerActor, bOwnerValid);
	if (!bOwnerValid || !OwnerNetID.IsValid())
		return false;

	bool bTargetOwnerValid = false;
	const FGuid TargetWriterID = GetCrowdyEntityWriterID(WorldContextObject, TargetActor, bTargetOwnerValid);

	const ECrowdyRole OwnerRole  = GetCrowdyEntityRole(WorldContextObject, OwnerActor);
	const ECrowdyRole TargetRole = GetCrowdyEntityRole(WorldContextObject, TargetActor);
	return UCrowdyEntityComponent::DoesOwnershipMatch(OwnerRole, OwnerNetID, TargetRole, TargetWriterID);
}

UCrowdyEntityComponent* UCrowdyUtilities::GetCrowdyEntityComponent(AActor* Actor)
{
	if (!IsValid(Actor))
		return nullptr;

	// Honour an explicit provider first (component on a child actor, runtime-selected, etc.).
	if (Actor->Implements<UCrowdyEntityComponentProvider>())
	{
		if (UCrowdyEntityComponent* Provided = ICrowdyEntityComponentProvider::Execute_GetCrowdyEntityComponent(Actor))
			return Provided;
	}

	return Actor->FindComponentByClass<UCrowdyEntityComponent>();
}

bool UCrowdyUtilities::IsCrowdyEntityLocallyOwned(UObject* Entity)
{
	// An entity actor's component already answers this, so the rule lives in one place. Resolving through GetCrowdyEntityComponent keeps the
	// ICrowdyEntityComponentProvider indirection working here too. A component, animation instance or other object
	// inside an actor, as Self from its own graph, asks about that actor.
	AActor* Actor = Cast<AActor>(Entity);
	if (!Actor && IsValid(Entity)) Actor = Entity->GetTypedOuter<AActor>();
	if (const UCrowdyEntityComponent* Component = GetCrowdyEntityComponent(Actor))
	{
		return Component->GetNetID().IsValid() && Component->IsLocallyOwned();
	}

	// A replicated subsystem has no component; its registry record answers the same way.
	const UWorld* World = IsValid(Entity) ? Entity->GetWorld() : nullptr;
	const UCrowdyEntitySubsystem* Entities = World ? World->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;
	const FCrowdyEntityRecord* Record = Entities ? Entities->FindRecord(Entities->FindEntityID(Entity)) : nullptr;
	return Record && Entities->IsRecordLocallyOwned(*Record);
}

bool UCrowdyUtilities::IsRecordLocallyOwned(const FCrowdyEntityRecord& Record, const FGuid& LocalPlayerID,
	const FGuid& WriterID)
{
	return FCrowdyEntityRecord::IsWrittenBy(Record.Role, Record.WriterID,LocalPlayerID, WriterID);
}

void UCrowdyUtilities::SwitchIsCrowdyEntityLocallyOwned(UObject* Entity, bool& bIsLocallyOwned)
{
	bIsLocallyOwned = IsCrowdyEntityLocallyOwned(Entity);
}

bool UCrowdyUtilities::IsCrowdyEntityOwnedByLocalUser(UObject* Entity)
{
	AActor* Actor = Cast<AActor>(Entity);
	if (!Actor && IsValid(Entity)) Actor = Entity->GetTypedOuter<AActor>();
	const UCrowdyEntityComponent* Component = GetCrowdyEntityComponent(Actor);
	return Component && Component->IsOwnedByLocalUser();
}

// Entity State Checks

void UCrowdyUtilities::SwitchIsCrowdyEntityPlayerControlled(UObject* WorldContextObject,
	AActor* TargetActor, bool& bIsPlayerControlled)
{
	bIsPlayerControlled = false;

	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("SwitchIsCrowdyEntityPlayerControlled"));
	if (!World) return;

	if (!IsValid(TargetActor))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdyUtilities]: SwitchIsCrowdyEntityPlayerControlled — TargetActor is null."));
		return;
	}

	// An entity that replicates movement = active participant (local player avatar or bot driving a state channel)
	const UCrowdyEntityComponent* Component = TargetActor->FindComponentByClass<UCrowdyEntityComponent>();
	if (Component && Component->OwnedBy == ECrowdyOwnedBy::Leader)
	{
		return;
	}

	if (Component && Component->ReplicatesMovement() && Component->GetRole() != ECrowdyRole::HostOwned)
	{
		bIsPlayerControlled = true;
		return;
	}

	// Registered in the entity registry = remote proxy of a player entity
	if (const UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>())
	{
		const FGuid EntityID = EntitySubsystem->FindEntityID(TargetActor);
		const FCrowdyEntityRecord* Record = EntitySubsystem->FindRecord(EntityID);
		if (Record && Record->Role == ECrowdyRole::RemoteProxy)
		{
			bIsPlayerControlled = true;
		}
	}
}

bool UCrowdyUtilities::IsCrowdyEntityPlayerControlled(UObject* WorldContextObject, AActor* TargetActor)
{
	bool bIsPlayerControlled = false;
	SwitchIsCrowdyEntityPlayerControlled(WorldContextObject, TargetActor, bIsPlayerControlled);
	return bIsPlayerControlled;
}

void UCrowdyUtilities::SwitchCrowdyIsRemoteProxy(UObject* WorldContextObject,
	AActor* Entity, bool& bIsRemoteProxy)
{
	bIsRemoteProxy = (GetCrowdyEntityRole(WorldContextObject, Entity) == ECrowdyRole::RemoteProxy);
}

bool UCrowdyUtilities::CrowdyIsRemoteProxy(UObject* WorldContextObject, const AActor* Entity)
{
	return GetCrowdyEntityRole(WorldContextObject, Entity) == ECrowdyRole::RemoteProxy;
}

// Events

void UCrowdyUtilities::SendCrowdyEvent_Internal(UObject* WorldContextObject, AActor* TargetEntity,
	FInstancedStruct EventPayload, ECrowdyEventRecipient Recipient,
	ECrowdyDecayRate DecayRate, ECrowdyReplicationDistance ReplicationDistance)
{
	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("SendCrowdyEvent"));
	if (!World) return;

	if (!IsValid(TargetEntity))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdyUtilities]: SendCrowdyEvent — TargetEntity is null."));
		return;
	}

	UCrowdyEntitySubsystem* EntitySubsystem = World->GetSubsystem<UCrowdyEntitySubsystem>();
	if (!IsValid(EntitySubsystem))
	{
		UE_LOG(LogCrowdyServices, Error, TEXT("[CrowdyUtilities]: SendCrowdyEvent — EntitySubsystem is null."));
		return;
	}

	const ECrowdyTarget Target = CrowdyUtils::MapRecipientToTarget(Recipient);
	EntitySubsystem->DispatchGameEvent(TargetEntity, MoveTemp(EventPayload), Target, TargetEntity, DecayRate, ReplicationDistance);
}

DEFINE_FUNCTION(UCrowdyUtilities::execK2_SendCrowdyEvent)
{
	P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
	P_GET_OBJECT(AActor, Z_Param_TargetEntity);

	// Wildcard struct: step manually so Blueprint can wire any struct type
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.MostRecentProperty        = nullptr;
	Stack.StepCompiledIn<FStructProperty>(nullptr);
	const void*      StructPtr  = Stack.MostRecentPropertyAddress;
	FStructProperty* StructProp = CastField<FStructProperty>(Stack.MostRecentProperty);

	P_GET_ENUM(ECrowdyEventRecipient, Z_Param_Recipient);
	P_GET_ENUM(ECrowdyDecayRate, Z_Param_DecayRate);
	P_GET_ENUM(ECrowdyReplicationDistance, Z_Param_ReplicationDistance);
	P_FINISH;

	P_NATIVE_BEGIN;
	if (ensureMsgf(StructProp && StructPtr, TEXT("[CrowdySDK] SendCrowdyEvent: EventPayload is not a valid struct.")))
	{
		FInstancedStruct EventPayload;
		EventPayload.InitializeAs(StructProp->Struct, static_cast<const uint8*>(StructPtr));
		SendCrowdyEvent_Internal(
			Z_Param_WorldContextObject,
			Z_Param_TargetEntity,
			MoveTemp(EventPayload),
			static_cast<ECrowdyEventRecipient>(Z_Param_Recipient),
			static_cast<ECrowdyDecayRate>(Z_Param_DecayRate),
			static_cast<ECrowdyReplicationDistance>(Z_Param_ReplicationDistance));
	}
	P_NATIVE_END;
}

// Session

bool UCrowdyUtilities::CrowdyIsConnectedToServer(UObject* WorldContextObject)
{
	UWorld* World = CrowdyUtils::GetWorldChecked(WorldContextObject, TEXT("CrowdyIsConnectedToServer"));
	if (!World) return false;

	const UGameInstance* GameInstance = World->GetGameInstance();
	if (!GameInstance) return false;

	const UCrowdyUDPSubsystem* UDP = GameInstance->GetSubsystem<UCrowdyUDPSubsystem>();
	if (!IsValid(UDP))
	{
		UE_CLOG(CrowdyServicesTrace::Services(), LogCrowdyServices, Log, TEXT("[CrowdyUtilities]: CrowdyIsConnectedToServer — UCrowdyUDPSubsystem is null."));
		return false;
	}

	return UDP->GetConnectionState() == EUDPConnectionState::Connected;
}
