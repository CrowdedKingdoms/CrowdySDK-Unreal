#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "CrowdyEntityComponentProvider.generated.h"

class UCrowdyEntityComponent;

UINTERFACE(Blueprintable, meta=(DisplayName="Crowdy Entity Component Provider"))
class CROWDYREPLICATION_API UCrowdyEntityComponentProvider : public UInterface
{
	GENERATED_BODY()
};

/**
 * Optional interface an actor (or any UObject) can implement to tell the SDK which
 * UCrowdyEntityComponent represents it - for example when the component lives on a
 * child actor, or is chosen at runtime rather than sitting directly on the actor.
 *
 * Implement it in C++ (override GetCrowdyEntityComponent_Implementation) or in
 * Blueprint (implement the interface event). It is intentionally NOT callable as a
 * node in a Blueprint graph (BlueprintInternalUseOnly): resolve the component through
 * UCrowdyUtilities::GetCrowdyEntityComponent, which consults this interface first and
 * falls back to FindComponentByClass when it is unimplemented (or returns null).
 */
class CROWDYREPLICATION_API ICrowdyEntityComponentProvider
{
	GENERATED_BODY()

public:

	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Entities", meta=(BlueprintInternalUseOnly="true"))
	UCrowdyEntityComponent* GetCrowdyEntityComponent() const;

	// Default: no explicitly-provided component; the utility resolver then falls back to FindComponentByClass.
	virtual UCrowdyEntityComponent* GetCrowdyEntityComponent_Implementation() const { return nullptr; }
};
