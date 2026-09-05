// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h" // the CrowdyContainer component these actors carry
#include "UObject/Object.h"
#include "CrowdyEntityClassContainerTestTypes.generated.h"

/**
 * Fixtures for binding a container from the class an entity records. They stand in for the two halves of one
 * entity: the game's own actor class, which declares the container, and the lightweight object that represents
 * the same entity on a machine that only observes it. CrowdyContainerTest keeps every one of them out of the real
 * schema sync and the baked registry.
 */

/** The game's own actor class: it declares the container, and it is the only place the author writes it. */
UCLASS(meta = (CrowdyContainer = "TestCrowdActor", CrowdyContainerTest))
class ACrowdyEntityClassContainerActor : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (CrowdyModel))
	int32 Hp = 100;
};

/** A second container actor whose author turned the initial pull off, so the pull decision has a wrong answer. */
UCLASS(meta = (CrowdyContainer = "TestCrowdActorNoPull", CrowdyContainerTest, CrowdyPullOnStart = "False"))
class ACrowdyEntityClassContainerNoPullActor : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (CrowdyModel))
	int32 Hp = 100;
};

/** An actor class that declares no container at all: an entity of this class has nothing to bind. */
UCLASS()
class ACrowdyEntityClassPlainActor : public AActor
{
	GENERATED_BODY()
};

/**
 * An actor whose server-owned state lives on a CrowdyContainer COMPONENT and not on the actor class itself.
 *
 * This is the shape a game reaches once health and stats move onto a reusable attribute component: the actor
 * declares no container, so a row standing in for one of these binds only what its components declare. The
 * component is a plain native default subobject, so its name is the same on the CDO and on every instance,
 * which is what lets an observer derive the id its owner minted.
 */
UCLASS()
class ACrowdyEntityClassComponentOwnerActor : public AActor
{
	GENERATED_BODY()

public:
	ACrowdyEntityClassComponentOwnerActor()
	{
		Attributes = CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes"));
	}

	UPROPERTY()
	TObjectPtr<UCrowdyGameModelTestComponent> Attributes;
};

/**
 * The same, plus a container declared on the actor class itself, so one entity holds two containers of
 * different types: its own, and the component's. What a row of a real player character looks like.
 */
UCLASS(meta = (CrowdyContainer = "TestCrowdBoth", CrowdyContainerTest))
class ACrowdyEntityClassBothContainersActor : public AActor
{
	GENERATED_BODY()

public:
	ACrowdyEntityClassBothContainersActor()
	{
		Attributes = CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes"));
	}

	UPROPERTY(meta = (CrowdyModel))
	int32 Score = 0;

	UPROPERTY()
	TObjectPtr<UCrowdyGameModelTestComponent> Attributes;
};

/**
 * An actor carrying more CrowdyContainer components than an entity may be stood in for.
 *
 * The class an entity records is chosen by that entity's own owner, and every container it names costs an
 * observer a binding and a retrying resolve, so the count one entity can spend is capped. Ten here against a
 * cap of eight, so the fixture stays wrong by a margin if the cap moves by one.
 */
UCLASS()
class ACrowdyEntityClassManyContainersActor : public AActor
{
	GENERATED_BODY()

public:
	ACrowdyEntityClassManyContainersActor()
	{
		// Distinct subobject names, because the derivation names a container by its instance term: ten names is
		// ten containers, which is the point.
		CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes00"));
		CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes01"));
		CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes02"));
		CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes03"));
		CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes04"));
		CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes05"));
		CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes06"));
		CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes07"));
		CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes08"));
		CreateDefaultSubobject<UCrowdyGameModelTestComponent>(TEXT("CrowdyTestAttributes09"));
	}
};

/**
 * What represents an observed entity on this machine when it is not drawn as an actor. It deliberately declares
 * nothing: everything about the entity's container has to come from the class the entity records.
 */
UCLASS()
class UCrowdyEntityClassStandIn : public UObject
{
	GENERATED_BODY()
};
