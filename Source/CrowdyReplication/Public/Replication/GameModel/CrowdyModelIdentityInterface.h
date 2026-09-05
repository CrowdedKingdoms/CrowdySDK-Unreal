#pragma once

#include "CoreMinimal.h"

/**
 * Cross-plane identity seam. Wraps UCrowdyEntitySubsystem + UCrowdyGameSession so Game Model code
 * (invoke client, container subsystem, effect lowering) depends on this interface instead of
 * reaching into CrowdyReplication/CrowdyNet internals directly. The interface IS the seam; a module
 * boundary is only its hard enforcement, so honoring it keeps a later extraction a cheap file move
 * instead of an untangling job.
 *
 * Plain C++ abstract class (not a UINTERFACE): every implementation and call site is native C++, so
 * there is no need for the UObject/Blueprint interface machinery.
 */
class CROWDYREPLICATION_API ICrowdyModelIdentity
{
public:
	virtual ~ICrowdyModelIdentity() = default;

	// Resolves Participant's NetID (an actor, or any other registered UObject participant per the
	// Subsystem Replication model). Returns false (OutNetID left untouched) when Participant is not a
	// registered entity.
	virtual bool GetNetIDForParticipant(const UObject* Participant, FGuid& OutNetID) const = 0;

	// True when NetID's registered owner is the local player.
	virtual bool IsLocallyOwned(const FGuid& NetID) const = 0;

	// Resolves the local player's own int64 Game Model user id. False when no session/user is known yet.
	virtual bool TryGetLocalUserId(int64& OutUserId) const = 0;

	// Resolves NetID's owner user id from the container subsystem's cache (filled as containers are
	// listed from the server). Returns false (Out=0) when no owner is cached yet for NetID.
	virtual bool TryGetCachedOwnerUserId(const FGuid& NetID, int64& Out) const = 0;

	virtual FGuid GetHostID() const = 0;
};
