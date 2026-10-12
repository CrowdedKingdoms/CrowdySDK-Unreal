#pragma once

#include "CoreMinimal.h"

struct CROWDYREPLICATION_API FCrowdyEntityIdentity
{
	// Deterministic NetID hashed from RemovePIEPrefix(path), so every client and edit-time tool derives the same id
	// for one level-placed actor. Fallback only: the primary derivation is the engine's ActorInstanceGuid, and this
	// path hash is not stable under World Partition.
	static FGuid StableNetIDFromActorPath(const FString& ActorPathName);

	// Deterministic NetID from an author-supplied binding key (see ICrowdyBindingKeyProvider). Caller guarantees a non-empty key.
	static FGuid NetIDFromBindingKey(const FString& Key);
};
