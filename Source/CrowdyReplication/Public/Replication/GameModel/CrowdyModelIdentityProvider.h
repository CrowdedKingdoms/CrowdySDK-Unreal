#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/CrowdyModelIdentityInterface.h"

class UCrowdyEntitySubsystem;
class UCrowdyGameModelSubsystem;
class UCrowdyGameSession;
class UWorld;

/**
 * Concrete ICrowdyModelIdentity backed directly by UCrowdyEntitySubsystem + UCrowdyGameSession --
 * never UCrowdyUtilities (CrowdyServices depends on CrowdyReplication, so the reverse dependency
 * would be circular). Resolves both subsystems once at construction, either from a UWorld context or
 * injected directly (the seam later unit tests / fakes use).
 *
 * Lifetime: construct per-use; do not let an instance outlive its source world / GameInstance. The
 * cached subsystem pointers are raw (a non-UObject cannot keep them GC-reachable), so a provider
 * retained across world teardown would dangle. A later phase needing a long-lived identity cache should
 * re-resolve per call or hold the subsystems behind a UObject / TWeakObjectPtr wrapper.
 */
class CROWDYREPLICATION_API FCrowdyModelIdentityProvider : public ICrowdyModelIdentity
{
public:
	explicit FCrowdyModelIdentityProvider(UWorld* InWorld);

	// Test/DI seam: build directly from already-resolved subsystems, bypassing the world lookup.
	FCrowdyModelIdentityProvider(UCrowdyEntitySubsystem* InEntitySubsystem, UCrowdyGameSession* InGameSession);

	virtual bool GetNetIDForParticipant(const UObject* Participant, FGuid& OutNetID) const override;
	virtual bool IsLocallyOwned(const FGuid& NetID) const override;
	virtual bool TryGetLocalUserId(int64& OutUserId) const override;
	virtual bool TryGetCachedOwnerUserId(const FGuid& NetID, int64& Out) const override;
	virtual FGuid GetHostID() const override;

private:
	void LogConstruction() const;

	UCrowdyEntitySubsystem* EntitySubsystem = nullptr;
	UCrowdyGameSession* GameSession = nullptr;
	// The container subsystem owns the NetID -> ownerUserId cache (filled from each entity's ensure/read result on
	// bind). Resolved in the world constructor only; null on the DI path (a test injects owners another way).
	// TryGetCachedOwnerUserId delegates here so the identity seam answers for a NetID once that entity has resolved.
	UCrowdyGameModelSubsystem* ModelSubsystem = nullptr;
};
