#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityHandle.h"
#include "CrowdyMassEntityTarget.generated.h"

class UWorld;

/**
 * Addresses one Mass-rendered remote player: which entity, and which world it lives in. Nothing else,
 * on purpose. Every other fact about the target (its identity, its owner, where it stands) is resolved
 * from the entity itself at send time rather than carried here, so it can never go stale relative to the
 * entity it names.
 *
 * FMassEntityHandle already carries a serial number that Mass bumps whenever a slot is reused, so a
 * handle held across a despawn and a later respawn under the same UUID does not silently start naming
 * the new entity: it fails the entity manager's IsEntityValid check exactly as any other stale handle
 * does. That protection is WITHIN ONE ENTITY MANAGER, which is why the world is carried beside the
 * handle rather than left implicit: serials restart per storage, so an index and serial from another
 * world can name a live entity here too, and only the world tells the two apart. A carried UUID would not have that property, and would also be a second, independently-arriving
 * copy of an identity the entity's own FCrowdyRemoteIdentityFragment already holds, which is what a
 * caller reading through the entity avoids disagreeing with.
 */
USTRUCT(BlueprintType)
struct CROWDYMASS_API FCrowdyMassEntityTarget
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Target")
	FMassEntityHandle Entity;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy Mass|Target")
	TWeakObjectPtr<UWorld> World;

	/**
	 * True when both halves of the target are present: an entity handle whose bits are set, and a world
	 * pointer that has not gone stale. This is a cheap structural check only, it says nothing about
	 * whether the entity manager still holds a live entity under this handle right now. Resolving the
	 * target against the entity manager (see CrowdyMassTarget::ResolveRpcTarget) is what answers that.
	 */
	bool IsSet() const
	{
		return Entity.IsSet() && World.IsValid();
	}
};
