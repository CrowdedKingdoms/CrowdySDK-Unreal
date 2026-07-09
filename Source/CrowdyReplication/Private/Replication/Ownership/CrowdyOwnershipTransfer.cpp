#include "Replication/Ownership/CrowdyOwnershipTransfer.h"
#include "CrowdyReplicationLog.h"

#include "Data/CrowdyEntityTypes.h"
#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

namespace
{
	// Resolves the entity subsystem (from the target's world) and the target's Crowdy entity component. Returns the
	// component and sets OutES only when both resolve; each node tailors its own log, so this stays quiet.
	UCrowdyEntityComponent* ResolveTarget(AActor* TargetEntity, UCrowdyEntitySubsystem*& OutES)
	{
		OutES = nullptr;
		if (!IsValid(TargetEntity))
			return nullptr;

		UWorld* World = TargetEntity->GetWorld();
		OutES = World ? World->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;
		if (!OutES)
			return nullptr;

		return TargetEntity->FindComponentByClass<UCrowdyEntityComponent>();
	}
}

void UCrowdyOwnershipTransfer::RequestOwnershipTransfer(UObject* /*WorldContextObject*/, AActor* TargetEntity)
{
	UCrowdyEntitySubsystem* ES = nullptr;
	UCrowdyEntityComponent* Component = ResolveTarget(TargetEntity, ES);
	if (!ES || !Component)
		return;

	const FGuid NetID = ES->FindEntityID(TargetEntity);
	if (!NetID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyOwnershipTransfer]: RequestOwnershipTransfer — '%s' is not a registered Crowdy entity."),
			*GetNameSafe(TargetEntity));
		return;
	}

	// Nothing to request when we are already this entity's authority. For a host-owned world entity that means we
	// are the host, which ES->IsLocallyOwned (owner-id based) would miss — so ask the component, whose
	// IsLocallyOwned covers both Owner and the host of a HostOwned entity.
	if (Component->IsLocallyOwned())
		return;

	Component->RequestOwnership(ES->GetLocalPlayerID());
}

void UCrowdyOwnershipTransfer::GrantOwnershipTransfer(UObject* WorldContextObject, AActor* TargetEntity, AActor* NewOwner)
{
	UCrowdyEntitySubsystem* ES = nullptr;
	UCrowdyEntityComponent* Component = ResolveTarget(TargetEntity, ES);
	if (!ES || !Component)
		return;

	if (!IsValid(NewOwner))
		return;

	// Resolve the player that owns NewOwner. A player avatar owns itself (NetID == OwnerID) and an entity a player
	// spawned carries that player's id in OwnerID, so either one names the intended player.
	const FGuid NewOwnerNetID = ES->FindEntityID(NewOwner);
	const FCrowdyEntityRecord* NewOwnerRecord = NewOwnerNetID.IsValid() ? ES->FindRecord(NewOwnerNetID) : nullptr;
	const FGuid NewOwnerPlayerID = NewOwnerRecord ? NewOwnerRecord->OwnerID : FGuid();

	if (!NewOwnerPlayerID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyOwnershipTransfer]: GrantOwnershipTransfer — '%s' is not a client-owned entity; use Grant Ownership To Host to make an entity host-owned."),
			*GetNameSafe(NewOwner));
		return;
	}

	GrantOwnershipTransferToPlayer(WorldContextObject, TargetEntity, NewOwnerPlayerID);
}

void UCrowdyOwnershipTransfer::GrantOwnershipTransferToPlayer(UObject* /*WorldContextObject*/, AActor* TargetEntity,
	FGuid NewOwnerPlayerID)
{
	UCrowdyEntitySubsystem* ES = nullptr;
	UCrowdyEntityComponent* Component = ResolveTarget(TargetEntity, ES);
	if (!ES || !Component)
		return;

	if (!NewOwnerPlayerID.IsValid())
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyOwnershipTransfer]: GrantOwnershipTransferToPlayer — invalid player id; use Grant Ownership To Host for host ownership."));
		return;
	}

	Component->GrantOwnershipTo(NewOwnerPlayerID);
}

void UCrowdyOwnershipTransfer::GrantOwnershipToHost(UObject* /*WorldContextObject*/, AActor* TargetEntity)
{
	UCrowdyEntitySubsystem* ES = nullptr;
	UCrowdyEntityComponent* Component = ResolveTarget(TargetEntity, ES);
	if (!ES || !Component)
		return;

	// An invalid new-owner id makes the entity host-owned (a world entity owned by whichever client is host).
	Component->GrantOwnershipTo(FGuid());
}
