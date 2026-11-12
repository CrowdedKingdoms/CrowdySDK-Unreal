#pragma once

#include "CoreMinimal.h"
#include "Subsystem/CrowdyActorOwners.h"
#include "UObject/Object.h"
#include "CrowdyActorOwnersTestTypes.generated.h"

/** Counts what a Resolve Actor Owner node's pins deliver. */
UCLASS(Transient, Hidden, meta=(CrowdyTestFixture))
class UCrowdyActorOwnersTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<FCrowdyActorOwner> Owners;

	UFUNCTION()
	void HandleOwner(const FCrowdyActorOwner& Owner) { Owners.Add(Owner); }
};
