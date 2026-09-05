#pragma once

#include "CoreMinimal.h"

class UCrowdyGameSession;

/**
 * Pure, lossless NetID <-> client-side container-key transforms, plus a LOCAL-ONLY int64 user-id
 * resolver. No UObject, no networking, no GraphQL.
 */
struct CROWDYREPLICATION_API FCrowdyModelIdentity
{
	// Pure, lossless NetID <-> client-side container key. The key is the canonical lowercase 32-hex
	// digest; it is the SDK-side stable handle, not necessarily the server container UUID (a later
	// phase's model component resolves/creates and caches that mapping).
	static FString NetIDToContainerKey(const FGuid& NetID);

	// Lossless inverse of NetIDToContainerKey. Returns false (OutNetID left untouched) on any
	// malformed input: wrong length or non-hex characters.
	static bool ContainerKeyToNetID(const FString& Key, FGuid& OutNetID);

	// The deterministic NetID a Stable-identity entity derives from its actor path. Hashing
	// RemovePIEPrefix(path) makes every client compute the same id for one level-placed actor, and lets an
	// edit-time tool reproduce the runtime id from an actor path alone. This is the single home for that
	// derivation so the runtime component and any tooling agree by construction.
	// Degenerate fallback only. The primary Stable-identity derivation is the engine's own per-placement
	// ActorInstanceGuid (see UCrowdyEntityComponent::ComputeStableNetID); this path hash is used only when an
	// actor has no engine guid at all, and is not stable under World Partition, which rewrites instance paths.
	static FGuid StableNetIDFromActorPath(const FString& ActorPathName);

	// The deterministic NetID an entity derives from an author-supplied binding key (see ICrowdyBindingKeyProvider):
	// GetDeterministicID(GenerateFromString(Key)). The single home for the key -> NetID transform so an actor and any
	// tooling agree by construction. Every client computes the same id from the same key, which is the whole point of
	// authoring a key when the engine gives no stable identity. Caller guarantees a non-empty key.
	static FGuid NetIDFromBindingKey(const FString& Key);

	// Local case ONLY. Resolves the int64 Game Model ownerUserId for an OwnerID FGuid when that
	// OwnerID is the LOCAL player (OwnerID == Session->GetID()). Returns false and OutUserID=0 for a
	// null Session or any remote owner: no FGuid<->int64 map exists for remote players until a later
	// phase caches gameModelContainers.ownerUserId against the NetID.
	static bool TryLocalUserIDForOwner(const UCrowdyGameSession* Session, const FGuid& OwnerID, int64& OutUserID);
};
