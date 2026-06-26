#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Interface.h"
#include "CrowdyReplicationSource.generated.h"

UINTERFACE(MinimalAPI, meta=(CannotImplementInterfaceInBlueprint))
class UCrowdyReplicationSource : public UInterface
{
	GENERATED_BODY()
};

/**
 * Something UCrowdyAutoReplicator can pull continuous state updates from.
 * Implemented by UCrowdyEntityComponent.
 */
class CROWDYREPLICATION_API ICrowdyReplicationSource
{
	GENERATED_BODY()

public:

	virtual AActor* GetReplicatedActor() const = 0;

	// String form of the entity NetID — the actor-update wire format still keys on it
	virtual const FString& GetReplicationUUID() const = 0;

	virtual const FInstancedStruct& GetReplicatedState() const = 0;
};
