// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyContainerStandIn.generated.h"

/**
 * Represents one CrowdyContainer component of an entity this machine draws as a row rather than as an actor.
 *
 * The entity registry maps one participant to one id, so an entity with two containers needs two participants and
 * the row's avatar can only be one of them. This is the other: an object with no behaviour whose only job is to
 * hold a registry record, so the component container the owner enrolled has something here to be addressed as.
 *
 * RepresentedClass is read directly rather than through the entity record's class id, and that is deliberate. A
 * class id is invertible only for a class the startup scan registered, which covers entity actor classes and not
 * the components hanging off them; resolving through it would leave the container unbound with nothing said. The
 * value is chosen locally from the class the entity records and never off a payload, so it routes and declares,
 * exactly as the recorded class does, and authorizes nothing.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyContainerStandIn : public UObject
{
	GENERATED_BODY()

public:
	/** The component class whose CrowdyContainer declaration this object stands for. */
	UPROPERTY(Transient)
	TObjectPtr<UClass> RepresentedClass;

	/** The per-instance term its id was derived from, kept so a diagnostic can name what it stands for. */
	UPROPERTY(Transient)
	FString InstanceTerm;
};
