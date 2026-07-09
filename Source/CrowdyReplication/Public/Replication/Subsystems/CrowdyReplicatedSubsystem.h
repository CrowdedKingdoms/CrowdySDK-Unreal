#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyReplicatedSubsystem.generated.h"

enum class ECrowdyOwnership : uint8;
class UWorld;

/**
 * Abstract base for a UWorldSubsystem that replicates as a CrowdySDK participant. Subclass it with a
 * concrete (non-abstract) type and its meta=(CrowdyState) properties auto-diff and its meta=(CrowdyEvent)
 * functions ride the reliable channel, with no hand-written RegisterParticipant/UnregisterParticipant. A
 * world subsystem's lifetime equals its world's, so it enrolls once in Initialize and unenrolls in
 * Deinitialize; there is no per-world re-enrollment (unlike the game-instance base).
 *
 * Scope: host-owned only (the default ownership). This is the client-authoritative VIEW plane. Host
 * precedence is a convention, not enforced. Authoritative or cheat-sensitive state belongs in a Game Model,
 * never here.
 */
UCLASS(Abstract)
class CROWDYREPLICATION_API UCrowdyReplicatedWorldSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

protected:

	// The authority axis this subsystem enrolls with. Host (the default) makes it a world singleton every client
	// agrees on by class path. Override to change it. Not a UFUNCTION, so the header needs only a forward
	// declaration of ECrowdyOwnership; the definition is in the .cpp, which has the complete enum.
	virtual ECrowdyOwnership GetReplicatedOwnership() const;
};

/**
 * Abstract base for a UGameInstanceSubsystem that replicates as a CrowdySDK participant. Same ergonomics as
 * UCrowdyReplicatedWorldSubsystem, but a game instance subsystem OUTLIVES individual worlds while the entity
 * registry it enrolls into is per-world. So it cannot enroll in its own Initialize (there is no world yet):
 * it re-enrolls into each new world's registry when that world's actors are initialized, and unenrolls as the
 * world is cleaned up. FWorldDelegates are process-global, so both handlers ignore worlds owned by a
 * different game instance.
 *
 * Same VIEW-plane guardrail as the world base: host-owned only, host precedence is convention, cheat-sensitive
 * state belongs in a Game Model.
 */
UCLASS(Abstract)
class CROWDYREPLICATION_API UCrowdyReplicatedGameInstanceSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

protected:

	virtual ECrowdyOwnership GetReplicatedOwnership() const;

private:

	// Per-world re-enrollment. OnWorldInitializedActors fires after a world's subsystems (entity registry + state
	// replicator) exist and have bound their delegates, so the enroll's registration broadcast is not missed;
	// OnWorldCleanup fires before the world's subsystem collection is torn down, so the registry still resolves
	// for the unenroll. Both ignore worlds not owned by this subsystem's game instance.
	void HandleWorldInitialized(UWorld* World);
	void HandleWorldCleanup(UWorld* World);

	FDelegateHandle WorldInitHandle;
	FDelegateHandle WorldCleanupHandle;
};
