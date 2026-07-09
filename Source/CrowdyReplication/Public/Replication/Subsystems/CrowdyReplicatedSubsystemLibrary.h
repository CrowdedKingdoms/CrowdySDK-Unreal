#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "CrowdyReplicatedSubsystemLibrary.generated.h"

class UCrowdyEntitySubsystem;

/**
 * Enrollment helpers that let a UE Subsystem participate in the CrowdySDK replication planes without
 * hand-wiring UCrowdyEntitySubsystem::RegisterParticipant. A host-owned subsystem enrolled here auto-diffs its
 * meta=(CrowdyState) properties and its meta=(CrowdyEvent) functions ride the reliable channel, exactly as an
 * actor entity does through its Crowdy Entity Component. Enrollment mints a network-stable NetID (a Host
 * participant's is the deterministic hash of its class path, identical on every client, no handshake).
 *
 * Two layers:
 *  - The injectable *Into core takes an explicit UCrowdyEntitySubsystem, so headless tests and the
 *    UGameInstanceSubsystem base (which resolves its own per-world registry) can enroll without a
 *    world-resolution step.
 *  - The BlueprintCallable wrappers resolve the entity subsystem from the caller's world (DefaultToSelf), for
 *    Blueprint and world-scoped callers. A null world, or a world with no entity subsystem (an editor/inactive
 *    world; the entity subsystem is PIE/Game only), is a clean no-op that never crashes.
 *
 * Scope is host-owned subsystems (Ownership defaults to Host): a singleton has no per-owner proxy on peers.
 * This is the client-authoritative view plane, where host precedence is a convention and not enforced, so
 * authoritative or cheat-sensitive state belongs in Game Models, never in a replicated subsystem property.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyReplicatedSubsystemLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Enrolls Subsystem into the given entity registry as a replicated participant and returns its
	 * network-stable NetID. Null-safe: an invalid Registry or Subsystem is a no-op that returns an invalid FGuid.
	 * The world-free core used by the ergonomic bases and the headless tests.
	 */
	static FGuid RegisterReplicatedSubsystemInto(UCrowdyEntitySubsystem* Registry, UObject* Subsystem, ECrowdyOwnership Ownership);

	/** Removes a participant previously enrolled via RegisterReplicatedSubsystemInto. Silent no-op if either is missing. */
	static void UnregisterReplicatedSubsystemInto(UCrowdyEntitySubsystem* Registry, UObject* Subsystem);

	/**
	 * Resolves the Crowdy Entity Subsystem from Subsystem's world and enrolls it as a replicated participant,
	 * returning its NetID (invalid FGuid on failure). Subsystem defaults to self, so a World Subsystem enrolls
	 * itself with no wiring. Do not call this from a Game Instance Subsystem: its world is the ambiguous "current"
	 * world, so enroll per-world through the *Into core instead.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Subsystem Replication", meta = (DefaultToSelf = "Subsystem"))
	static FGuid RegisterReplicatedSubsystem(UObject* Subsystem, ECrowdyOwnership Ownership = ECrowdyOwnership::Host);

	/** Resolves the Crowdy Entity Subsystem from Subsystem's world and unenrolls it. Clean no-op when unresolved. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Subsystem Replication", meta = (DefaultToSelf = "Subsystem"))
	static void UnregisterReplicatedSubsystem(UObject* Subsystem);
};
