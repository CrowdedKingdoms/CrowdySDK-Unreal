#pragma once

#include "CoreMinimal.h"
#include "FGameVersion.generated.h"

USTRUCT(BlueprintType)
struct FGameVersion
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Version = 1;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Game Version")
	int32 MajorVersion = 0;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Game Version")
	int32 MinorVersion = 0;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Game Version")
	int32 PatchVersion = 0;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Game Version")
	int32 BuildNumber = 0;


};