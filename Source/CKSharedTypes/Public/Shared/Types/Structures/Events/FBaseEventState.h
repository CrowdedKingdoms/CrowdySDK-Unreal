#pragma once
#include "CoreMinimal.h"
#include "FBaseEventState.generated.h"


USTRUCT(BlueprintType, Blueprintable)
struct FBaseEventState
{
	GENERATED_BODY()
	
	// Common properties all events might have
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game Event")
	FVector Location = FVector(0.0f, 0.0f, 0.0f);
    
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game Event")
	FRotator Rotation = FRotator(0.0f, 0.0f, 0.0f);
    
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game Event")
	FVector Scale = FVector(1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game Event")
	int64 Timestamp = 0;
	
	// Virtual destructor for proper cleanup
	virtual ~FBaseEventState() {}

};

USTRUCT(BlueprintType, Blueprintable)
struct FBallState : public FBaseEventState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game Event")
	uint8 Version = 1;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game Event")
	FVector Velocity = FVector(0.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Game Event")
	float InitialSpeed = 0.0f;
};