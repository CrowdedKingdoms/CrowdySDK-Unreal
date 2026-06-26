#pragma once
#include "CoreMinimal.h"
#include "FCrowdySelectIDAsHost.generated.h"

USTRUCT(BlueprintType)
struct FCrowdySelectIDAsHost
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadWrite, Category="Crowdy SDK|Events|Internal")
	FGuid NewHostID;
};