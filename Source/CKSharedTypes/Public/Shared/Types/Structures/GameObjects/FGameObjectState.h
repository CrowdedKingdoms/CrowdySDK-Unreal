#pragma once
#include "CoreMinimal.h"
#include "FGameObjectState.generated.h"

USTRUCT(BlueprintType, Blueprintable)
struct FGameObjectState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadWrite, Category = "Game Object|State")
	uint8 Version = 1;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Game Object|State")
	bool bIsActive = false;

	virtual ~FGameObjectState() {}
};