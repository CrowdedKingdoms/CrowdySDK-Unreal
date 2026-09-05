#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyEntityTypes.h"
#include "UObject/Interface.h"
#include "ICrowdyEventSource.generated.h"

UINTERFACE(MinimalAPI, meta=(CannotImplementInterfaceInBlueprint))
class UCrowdyEventSource : public UInterface
{
	GENERATED_BODY()
};

/**
 * Lets a plain UObject send a CrowdyEvent.
 *
 * The normal send path routes by the ACTOR that carries the entity identity: it reads the actor's
 * registered NetID for addressing and the actor's world location for the spatial transport. An object
 * that is neither an actor nor a component of one has neither, so it implements this interface and
 * answers those questions itself. That is the whole purpose: to represent an entity that has no actor,
 * such as one simulated as a row in a table rather than as a spawned actor.
 *
 * Every answer must describe the ENTITY this object stands for, read at the moment it is asked. In
 * particular GetEventLocation must report where that entity is right now, from the entity's own
 * simulation state. Never let a caller pass a location in and hand that back: a caller-supplied
 * position lets one entity claim to be anywhere, which is exactly what deriving the position from an
 * actor's transform prevented.
 *
 * Implementing this interface does not change what an actor does. The send path only consults it for a
 * source that has no owning actor, so an actor or component that also implements it still routes by
 * its actor identity.
 *
 * All four recipients can be sent. Owner-only and host-only reach their single recipient over a
 * transport that addresses a registered id plus a chunk, not an actor, so a source with no actor uses
 * it like any other sender. Two of the four do need a position, and drop with a warning when
 * GetEventLocation reports none: Spatial Multicast, which announces from the region the entity stands
 * in, and owner-only when the entity is owned by another client, which is addressed to the chunk the
 * entity stands in. Multicast never needs one, and neither does host-only, which is addressed from
 * where the HOST stands rather than from where this source is. An owner-only event on a world entity
 * that no client owns is also dropped, since there is no single client to deliver it to; use host-only
 * for a world entity.
 *
 * One further limit: a source may only originate an event for an entity this client is the authority
 * for, which means one it owns, or a world entity while this client is the host.
 */
class CROWDYREPLICATION_API ICrowdyEventSource
{
	GENERATED_BODY()

public:

	// The entity this object speaks for, as its network id. This is the id the receiving client resolves
	// the event against, so it must be the same id every other client knows the entity by. An invalid
	// guid means "no identity yet"; the send is dropped, because there is nothing for a receiver to
	// resolve.
	virtual FGuid GetEventEntityID() const = 0;

	// Where the entity is right now, in world space. Return false when that is genuinely unknown (the
	// entity has no position yet, or its simulation state is not readable at this instant) and leave
	// OutLocation untouched; a send that addresses a region is then dropped rather than sent from a
	// made-up position, which would place the event in the wrong part of the world for every receiver.
	// Only the sends that address a region read this, so returning false does not stop the others.
	virtual bool GetEventLocation(FVector& OutLocation) const = 0;

	// The client that owns the entity, as its player id. This is the identity stamped on the event as its
	// sender. An invalid guid means no client owns it: it is a world entity belonging to whichever client
	// is currently the host, and the send path then stamps the local player instead.
	virtual FGuid GetEventOwnerID() const = 0;

	// This client's relationship to the entity: Owner when this client simulates and sends for it,
	// RemoteProxy when it only receives, HostOwned for a world entity, None when it is not registered.
	virtual ECrowdyRole GetEventRole() const = 0;

	// True when this client is the entity's owner. A source that is not locally owned may still receive
	// events; it just may not originate them, because an event sent for an entity this client does not own
	// would carry this client's identity on another client's entity. The one exception is a world entity,
	// which no client owns and which the host may therefore send for; the send path reads GetEventRole to
	// recognise that case.
	virtual bool IsEventLocallyOwned() const = 0;
};
