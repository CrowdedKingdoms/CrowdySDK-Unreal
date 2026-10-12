#include "Mass/CrowdyMassEntity.h"

#include "Engine/World.h"
#include "Fragments/CrowdyMassAnimFragments.h"
#include "Fragments/CrowdyMassFragments.h"
#include "Mass/CrowdyMassActionEvents.h"
#include "Mass/EntityFragments.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"

void UCrowdyMassEntity::BindToEntity(UCrowdyMassEntitySubsystem* InOwner, const FMassEntityHandle InHandle, const FGuid& InUUID)
{
	Owner = InOwner;
	Handle = InHandle;
	UUID = InUUID;
}

bool UCrowdyMassEntity::IsValid() const
{
	const FMassEntityManager* EntityManager = GetEntityManager();
	return EntityManager && EntityManager->IsEntityValid(Handle);
}

FGuid UCrowdyMassEntity::GetEventEntityID() const
{
	// The id is reported even for an entity that has since been destroyed, because it is the id this
	// object was minted for and answering with a different one would be worse than answering with a
	// stale one. Whether the entity still exists is a separate question, asked through IsValid.
	return UUID;
}

bool UCrowdyMassEntity::GetEventLocation(FVector& OutLocation) const
{
	const FTransformFragment* TransformFragment = Frag<FTransformFragment>();
	if (!TransformFragment)
	{
		return false;
	}

	const FVector Location = TransformFragment->GetTransform().GetLocation();
	if (Location.ContainsNaN())
	{
		// An entity whose position has gone non-finite has no place in the world to send from, and the
		// spatial transport would turn one into a region no receiver is in, or every region at once.
		return false;
	}

	OutLocation = Location;
	return true;
}

FGuid UCrowdyMassEntity::GetEventOwnerID() const
{
	const FCrowdyRemoteIdentityFragment* Identity = Frag<FCrowdyRemoteIdentityFragment>();
	return Identity ? Identity->OwnerID : FGuid();
}

ECrowdyRole UCrowdyMassEntity::GetEventRole() const
{
	const FCrowdyRemoteIdentityFragment* Identity = Frag<FCrowdyRemoteIdentityFragment>();
	if (!Identity)
	{
		// No identity fragment means the entity is gone or was never built as a Crowdy remote entity.
		// Either way nothing here is registered, which is exactly what None reports.
		return ECrowdyRole::None;
	}

	return Identity->Role;
}

void UCrowdyMassEntity::PlayCrowdyAction_Implementation(const int32 ActionId, const int32 Variant, const float PlayRate)
{
	FCrowdyMassActionFragment* Action = Frag<FCrowdyMassActionFragment>();
	if (!Action)
	{
		// An entity built from a config with no animation trait carries no action slot, so there is
		// nothing an action could be played on. Ordinary rather than a fault, and silent here because a
		// crowd of them would print a line per entity per event.
		return;
	}

	// The same write every other road to an action performs, so an action looks identical whichever one
	// it travelled and the forged-value bounds cannot drift between them.
	CrowdyMassWriteAction(*Action, ActionId, Variant, PlayRate);
}

bool IsCrowdyMassIdentityLocallyOwned(const ECrowdyRole Role, const FGuid& OwnerID, const FGuid& LocalPlayerID)
{
	// Owner and nothing else. A host-owned entity is not owned by any client, and the send path
	// recognises that case on its own by reading the role; folding it in here would also let a client
	// that is not the host speak for one.
	if (Role != ECrowdyRole::Owner)
	{
		return false;
	}

	// No owner recorded is not a disagreement. The send path substitutes the local player as the sender
	// in exactly this case, so the entity does speak as this client and the role's claim stands.
	if (!OwnerID.IsValid())
	{
		return true;
	}

	return OwnerID == LocalPlayerID;
}

bool UCrowdyMassEntity::IsEventLocallyOwned() const
{
	const FCrowdyRemoteIdentityFragment* Identity = Frag<FCrowdyRemoteIdentityFragment>();
	if (!Identity)
	{
		return false;
	}

	// Who this client is only matters when an owner id is actually claimed, so the world is resolved for
	// that case alone and an entity whose role settles the question needs none.
	FGuid LocalPlayerID;
	if (Identity->Role == ECrowdyRole::Owner && Identity->OwnerID.IsValid())
	{
		const UCrowdyMassEntitySubsystem* Subsystem = Owner.Get();
		const UWorld* World = Subsystem ? Subsystem->GetWorld() : nullptr;
		const UCrowdyEntitySubsystem* Entities = World ? World->GetSubsystem<UCrowdyEntitySubsystem>() : nullptr;
		if (!Entities)
		{
			// Nothing to compare against. Answering yes here would make an unresolvable world the way
			// around the check, so a question that cannot be answered is a refusal.
			return false;
		}

		LocalPlayerID = Entities->GetLocalPlayerID();
	}

	return IsCrowdyMassIdentityLocallyOwned(Identity->Role, Identity->OwnerID, LocalPlayerID);
}

FMassEntityManager* UCrowdyMassEntity::GetEntityManager() const
{
	const UCrowdyMassEntitySubsystem* Subsystem = Owner.Get();
	return Subsystem ? Subsystem->GetMassEntityManager() : nullptr;
}
