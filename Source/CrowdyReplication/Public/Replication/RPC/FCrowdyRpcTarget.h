#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyEntityTypes.h" // ECrowdyRole
#include "Misc/Optional.h"

/**
 * A replicated entity addressed by identity rather than by a live UObject: everything the RPC send
 * path needs in order to route a call to an entity that this client may hold only as rendering data
 * (a row in a crowd simulation) instead of as an actor.
 *
 * Every field describes the TARGET and must be read from the target's own live state at the moment it
 * is resolved. Location in particular is where the entity stands right now, taken from whatever holds
 * its transform. It decides which region of the world the message is addressed to, so a location that
 * arrived from further up the call chain, or that whoever asked for the send handed in, would let a
 * caller address a message anywhere in the world. Resolve it; never accept it.
 *
 * Where this client already holds a registration for the entity, the send path prefers that record's
 * owner, role and (when it holds an actor) position over what is filled in here, so the two views can
 * never disagree about who owns the target. What is filled in here is what decides the routing for an
 * entity this client holds no registration for, which is the case this type exists for.
 *
 * The target is not the sender. A call routed to a target still goes out as this client: the send path
 * stamps the local player as the sender and never takes a sending identity from here.
 */
struct FCrowdyRpcTarget
{
	// The entity's network id, the same id every other client knows it by. This is what the server
	// routes a targeted message by, and what the receiver resolves the call against in its own registry.
	FGuid NetID;

	// Where the entity is right now, in world space. Unset means its position is genuinely unknown at
	// this instant, which only stops the routes that address a region: a call addressed to a specific
	// client, or one that runs here and goes nowhere, never reads it.
	TOptional<FVector> Location;

	// The client that owns the entity, as its player id. Invalid for a world entity no client owns.
	FGuid OwnerID;

	// This client's relationship to the entity: Owner when this client simulates and sends for it,
	// RemoteProxy when it only mirrors or renders it, HostOwned for a world entity.
	ECrowdyRole Role = ECrowdyRole::None;

	// True when the target names an entity a receiver could resolve. Without one there is nothing to
	// address and nothing to route.
	bool IsRoutable() const
	{
		return NetID.IsValid();
	}

	// True when a position is available to address a region with.
	bool HasLocation() const
	{
		return Location.IsSet();
	}

	// True when PlayerID is this entity's owner. The id and the role both have to agree, mirroring
	// UCrowdyEntitySubsystem::IsLocallyOwned: an owner id can arrive from a decoded message, while the
	// role is only ever set by whatever registered the entity on this client, so either one alone is
	// not an answer.
	bool IsOwnedBy(const FGuid& PlayerID) const
	{
		return Role == ECrowdyRole::Owner && PlayerID.IsValid() && OwnerID == PlayerID;
	}

	// True when no client owns this entity, so there is no single client a targeted message could be
	// delivered to. A world entity belongs to whichever client is host, which the Host recipient
	// addresses; the owner-only recipient has no destination for it at all.
	bool BelongsToNoClient() const
	{
		return Role == ECrowdyRole::HostOwned;
	}
};
