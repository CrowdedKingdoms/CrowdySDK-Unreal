#pragma once
#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "StructUtils/InstancedStruct.h"
#include "FGameEventNotificationBP.generated.h"

USTRUCT(BlueprintType, meta=(DisplayName="Game Event Notification"))
struct FGameEventNotificationBP
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int64 ChunkX;
	
	UPROPERTY(BlueprintReadOnly)
	int64 ChunkY;
	
	UPROPERTY(BlueprintReadOnly)
	int64 ChunkZ;
	
	UPROPERTY(BlueprintReadOnly)
	FString UUID;

	UPROPERTY(BlueprintReadOnly)
	FInstancedStruct Event;
	
	UPROPERTY(BlueprintReadOnly)
	int64 Timestamp;
};
