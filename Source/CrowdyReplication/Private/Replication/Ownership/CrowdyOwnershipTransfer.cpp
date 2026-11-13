#include "Replication/Ownership/CrowdyOwnershipTransfer.h"
#include "CrowdyReplicationLog.h"

#include "Data/CrowdyEntityTypes.h"
#include "Engine/World.h"
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

	// Nothing to request when we are already this entity's authority.
	if (Component->IsLocallyOwned())
		return;

	if (Component->OwnedBy == ECrowdyOwnedBy::Leader)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyOwnershipTransfer]: RequestOwnershipTransfer ignored: '%s' is Owned By Leader, so its owner follows the Leader."),
			*GetNameSafe(TargetEntity));
		return;
	}

	const FCrowdyEntityRecord* Record = ES->FindRecord(NetID);
	if (Record && Record->Role == ECrowdyRole::HostOwned)
	{
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyOwnershipTransfer]: RequestOwnershipTransfer ignored: '%s' is a World entity, so no client writes it to grant it."),
			*GetNameSafe(TargetEntity));
		return;
	}

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
			TEXT("[CrowdyOwnershipTransfer]: GrantOwnershipTransfer — '%s' is not a client-owned entity."),
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
			TEXT("[CrowdyOwnershipTransfer]: GrantOwnershipTransferToPlayer — invalid player id."));
		return;
	}

	Component->GrantOwnershipTo(NewOwnerPlayerID);
}
