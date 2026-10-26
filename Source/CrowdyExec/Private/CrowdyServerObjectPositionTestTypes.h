#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Controller.h"
#include "UObject/Object.h"
#include "CrowdyServerObjectPositionTestTypes.generated.h"

/** The inputs of a position function: the ground position in metres. */
USTRUCT()
struct FCrowdyServerObjectTestMoveInputs
{
	GENERATED_BODY()

	UPROPERTY()
	double X = 0.0;

	UPROPERTY()
	double Y = 0.0;
};

/** The position the server kept. */
USTRUCT()
struct FCrowdyServerObjectTestMoveReply
{
	GENERATED_BODY()

	UPROPERTY()
	double X = 0.0;

	UPROPERTY()
	double Y = 0.0;
};

/** Inputs with an X and a Y that are not doubles. */
USTRUCT()
struct FCrowdyServerObjectTestIntegerMoveInputs
{
	GENERATED_BODY()

	UPROPERTY()
	int32 X = 0;

	UPROPERTY()
	int32 Y = 0;
};

/** A controller that is not a player's, as an AI's is. */
UCLASS(Transient, NotBlueprintable)
class ACrowdyServerObjectPositionTestController : public AController
{
	GENERATED_BODY()
};

/** Records the positions a component reports as kept. */
UCLASS(Transient)
class UCrowdyServerObjectPositionTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<FVector> Kept;

	UFUNCTION()
	void HandleKept(FVector Location)
	{
		Kept.Add(Location);
	}
};
