#pragma once

#include "CoreMinimal.h"
#include "CrowdyServerObjectComponent.h"
#include "GameFramework/Actor.h"
#include "Mass/EntityHandle.h"
#include "Target/CrowdyMassEntityTarget.h"
#include "UObject/Object.h"
#include "CrowdyMassServerRecordsTestTypes.generated.h"

/** One crowd entity's record in the server records tests. Not wrapped in a test guard: reflected types cannot be. */
USTRUCT()
struct FCrowdyMassServerRecordsTestRecord
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Health = 100;

	UPROPERTY()
	int32 Ammo = 0;
};

/** An actor class carrying the Crowdy Server Object component natively, as a C++ game class does. */
UCLASS(Hidden)
class ACrowdyMassServerRecordsTestActor : public AActor
{
	GENERATED_BODY()

public:
	ACrowdyMassServerRecordsTestActor();

	UPROPERTY()
	TObjectPtr<UCrowdyServerObjectComponent> Records;
};

/** Stands in for the object a Receive Server Values Changed on Entity node binds. */
UCLASS(Hidden)
class UCrowdyMassServerRecordsTestHandler : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION()
	void OnChanged(FCrowdyMassEntityTarget Target, const TArray<FString>& Changed);

	UFUNCTION()
	void OnNearTierChanged(const TArray<FString>& Changed);

	UFUNCTION()
	void WrongShape(int32 Value);

	int32 Calls = 0;
	FMassEntityHandle LastEntity;
	TArray<FString> LastChanged;
	int32 NearTierCalls = 0;
	TArray<FString> NearTierLastChanged;
};
