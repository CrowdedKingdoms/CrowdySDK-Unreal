#pragma once

#include "CoreMinimal.h"
#include "FTextMessage.generated.h"

USTRUCT(Blueprintable, BlueprintType)
struct FTextMessage
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Version = 1;
	
	UPROPERTY(BlueprintReadWrite, Category = "Text Message")
	int64 UserID;
	
	UPROPERTY(BlueprintReadWrite, Category = "Text Message")
	FString Username;

	UPROPERTY(BlueprintReadWrite, Category = "Text Message")
	FString Message;
};