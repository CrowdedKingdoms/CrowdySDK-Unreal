#pragma once

#include "CoreMinimal.h"
#include "FOriginOffset.generated.h"

USTRUCT(BlueprintType, Blueprintable)
struct FOriginOffset
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Origin Offset")
	int64 OffsetX;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Origin Offset")
	int64 OffsetY;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Origin Offset")
	int64 OffsetZ;

	FOriginOffset():
	OffsetX(0), OffsetY(0), OffsetZ(0){};
};