#pragma once

#include "CoreMinimal.h"

struct FCrowdyMassEntityTarget;
struct FCrowdyRpcTarget;
struct FMassEntityManager;
class UFunction;
class UClass;

/**
 * Turns an FCrowdyMassEntityTarget into the routing state the RPC send path needs, and answers whether a
 * function may be sent to the entity it names.
 *
 * FCrowdyRpcTarget is not a USTRUCT (it exists purely to carry C++-resolved routing state), so this stays
 * a plain C++ entry point. Nothing here is reachable from a Blueprint graph directly; only
 * UCrowdyMassEventLibrary's thunk calls it.
 */
namespace CrowdyMassTarget
{
	/**
	 * Why ResolveRpcTarget could not produce a routable target, or that it did. A dead world, a stale
	 * handle, a missing identity fragment, and an entity with no routable identity are four different
	 * situations a caller (and a test) needs to tell apart rather than one bare false: the first three
	 * are "this entity is not ready yet", and the last is "this entity can never be addressed, something
	 * is wrong with how it was spawned".
	 */
	enum class EResolveResult : uint8
	{
		Resolved,        // OutRpcTarget is filled in and routable.
		NoWorld,         // Target.World has gone, or this world holds no Mass entity manager.
		InvalidEntity,   // The entity manager does not consider Target.Entity a live entity right now.
		MissingIdentity, // The entity is live but carries no FCrowdyRemoteIdentityFragment.
		Unroutable,      // The identity fragment's UUID is not a valid guid, so nothing could address it.
	};

	/**
	 * Resolves Target against its own world's Mass entity manager: IsEntityValid, one
	 * FCrowdyRemoteIdentityFragment read (UUID, OwnerID and Role together), and one FTransformFragment
	 * read for Location. Nothing more: no FindEntityByUUID, no hash keyed on the UUID, no subsystem
	 * search beyond getting the entity manager itself, since the entity handle already is the key.
	 *
	 * OutRpcTarget.Location is left unset, not defaulted to the origin, when the entity carries no
	 * FTransformFragment: an unset location and a location at the world origin address different things
	 * to the RPC send path, and only one of them is true here.
	 *
	 * OutRpcTarget is only meaningful when the result is Resolved. On any other result it may be left
	 * partially filled and callers must not use it. Game thread only.
	 *
	 * THIS is the form to call. It reads Target.World, and the world is what says which entity manager the
	 * handle is an index into. Handles are not unique across managers: a slot's serial restarts per
	 * storage, so the same index and serial can name a live entity in two different worlds at once.
	 */
	CROWDYMASS_API EResolveResult ResolveRpcTarget(const FCrowdyMassEntityTarget& Target, FCrowdyRpcTarget& OutRpcTarget);

	/**
	 * ResolveRpcTarget against an entity manager the caller supplies, which is where all of the work
	 * actually lives. Target.World is NOT read here, so this never answers NoWorld.
	 *
	 * That makes it the wrong entry point for ordinary gameplay code, and it is exported for the case
	 * where there is no world to read: the manager is otherwise reached through a world subsystem that
	 * exists only in a PIE or Game world, which would put every outcome past that lookup out of reach of
	 * a test. Call it only when you know the manager and the target belong together, because nothing
	 * here can check that for you: an index and serial from another world can match a live entity in this
	 * one and resolve to the wrong player.
	 */
	CROWDYMASS_API EResolveResult ResolveRpcTargetIn(FMassEntityManager& EntityManager,
		const FCrowdyMassEntityTarget& Target, FCrowdyRpcTarget& OutRpcTarget);

	/**
	 * Whether a call to Fn may be sent to an entity whose locally recorded class is RecordedClass.
	 *
	 * A function declared on a base of RecordedClass passes: an event declared on a C++ base with a
	 * Blueprint-derived recorded class is the ordinary case, not a mismatch, so the rule is
	 * RecordedClass->IsChildOf(Fn's declaring class), not equality.
	 *
	 * An event declared on a UActorComponent or a UCrowdyMassEntity also passes: those are what the
	 * receiving side can resolve to besides the participant itself, and the class cannot say whether an
	 * instance will carry one. An event declared anywhere else the entity is not is still refused.
	 *
	 * This answers an AUTHORING question, not a trust one. RecordedClass ultimately comes from the ClassID
	 * an entity's own sender put on the update that announced it, so it says what the entity claims to be,
	 * not what it provably is. Use it to tell a developer they have aimed an event at the wrong kind of
	 * entity; never to decide anything a peer must not be able to influence.
	 *
	 * RecordedClass is null exactly when this client has not yet learned the entity's class (see
	 * UCrowdyMassEntitySubsystem::GetEntityClass). That is a different situation from a class that
	 * has arrived and does not derive from Fn's declaring class: one is "not ready yet", the other is
	 * "this event does not belong on this entity at all", and they are reported as different results
	 * rather than sharing one refusal.
	 */
	enum class EClassCheckResult : uint8
	{
		Ok,            // RecordedClass is Fn's declaring class, or derives from it.
		ClassUnknown,  // RecordedClass is null: nothing has told this client the entity's class yet.
		ClassMismatch, // RecordedClass is known and does not derive from Fn's declaring class.
	};

	CROWDYMASS_API EClassCheckResult CheckEventClass(const UFunction* Fn, const UClass* RecordedClass);
}
