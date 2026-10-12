#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "CrowdyBindingKeyProvider.generated.h"

UINTERFACE(BlueprintType, meta = (DisplayName = "Crowdy Binding Key Provider"))
class CROWDYREPLICATION_API UCrowdyBindingKeyProvider : public UInterface
{
	GENERATED_BODY()
};

/**
 * Optional per-instance identity. An actor implements this to supply a stable, cross-client-identical key naming
 * THIS instance for the case the engine cannot identify automatically: an independently client-spawned world
 * object (no level placement, no SDK spawn id). The SDK folds the key into the instance's NetID instead of the
 * default derivation (the actor's IdentityPolicy).
 *
 * Contract: an empty return falls back to the default derivation (so implementing it and returning "" is a no-op).
 * Two distinct instances MUST return distinct keys, and the SAME logical instance MUST return the SAME key on every
 * client - derive it from game semantics ("arena2_boss", a shared spawn seed), never from a per-process counter,
 * a pointer, or local spawn order. Cooked-safe (an interface, not metadata); implementable in C++ or Blueprint.
 *
 * TIMING: the key is read when the entity resolves its identity, inside UCrowdyEntityComponent::BeginPlay - which
 * for a component runs BEFORE the owning actor's own BeginPlay event graph. So the key must already be set by then:
 * a spawn parameter, a UPROPERTY default, or the construction script. A key COMPUTED in the actor's BeginPlay graph
 * is not yet available and reads as empty (silently falling back to the default derivation).
 *
 * NAMESPACE: the key is a GLOBAL identifier (it alone derives the NetID), so it must be unique across every actor
 * in the app. When in doubt, make keys fully qualified ("arena2_boss", not "boss").
 */
class CROWDYREPLICATION_API ICrowdyBindingKeyProvider
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Crowdy SDK|Entity",
		meta = (DisplayName = "Get Crowdy Binding Key"))
	FString GetCrowdyBindingKey() const;
};
