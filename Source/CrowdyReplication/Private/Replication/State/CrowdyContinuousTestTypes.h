#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Replication/Components/CrowdyEntityComponent.h"
#include "CrowdyContinuousTestTypes.generated.h"

/** A class that replicates movement, with one ordinary property and two continuous ones. */
UCLASS(meta = (CrowdyTestFixture))
class ACrowdyContinuousTestActor : public AActor
{
	GENERATED_BODY()

public:

	ACrowdyContinuousTestActor()
	{
		Entity = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("Entity"));
		Entity->ReplicateMovement = ECrowdyReplicateMovement::On;
	}

	UPROPERTY(meta = (CrowdyState))
	int32 RepPlain = 0;

	UPROPERTY(meta = (CrowdyState, CrowdyContinuous, CrowdyOnRep = "OnRep_Lean"))
	float RepLean = 0.f;

	UPROPERTY(meta = (CrowdyState, CrowdyContinuous, CrowdyHeartbeat))
	int32 RepStance = 0;

	UPROPERTY()
	int32 LeanOnRepCount = 0;

	UPROPERTY()
	TObjectPtr<UCrowdyEntityComponent> Entity;

	UFUNCTION()
	void OnRep_Lean() { ++LeanOnRepCount; }
};

/** Marked continuous on a class that sends no movement, so nothing would ever carry it. */
UCLASS(meta = (CrowdyTestFixture))
class ACrowdyContinuousStillTestActor : public AActor
{
	GENERATED_BODY()

public:

	ACrowdyContinuousStillTestActor()
	{
		Entity = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("Entity"));
		Entity->ReplicateMovement = ECrowdyReplicateMovement::Off;
	}

	UPROPERTY(meta = (CrowdyState, CrowdyContinuous))
	float RepLean = 0.f;

	UPROPERTY()
	TObjectPtr<UCrowdyEntityComponent> Entity;
};

/** The two markers a movement update cannot honour. */
UCLASS(meta = (CrowdyTestFixture))
class ACrowdyContinuousConflictTestActor : public AActor
{
	GENERATED_BODY()

public:

	ACrowdyContinuousConflictTestActor()
	{
		Entity = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("Entity"));
		Entity->ReplicateMovement = ECrowdyReplicateMovement::On;
	}

	UPROPERTY(meta = (CrowdyState, CrowdyContinuous, CrowdyOwnerOnly))
	float RepSecret = 0.f;

	UPROPERTY(meta = (CrowdyState, CrowdyContinuous, CrowdyManualDirty))
	float RepManual = 0.f;

	UPROPERTY()
	TObjectPtr<UCrowdyEntityComponent> Entity;
};
