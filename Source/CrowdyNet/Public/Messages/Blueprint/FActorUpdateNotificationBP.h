#pragma once
#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "StructUtils/InstancedStruct.h"
#include "FActorUpdateNotificationBP.generated.h"

USTRUCT(BlueprintType, meta=(DisplayName="Actor Update Notification"))
struct FActorUpdateNotificationBP
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
	FInstancedStruct ActorUpdate;
	
	UPROPERTY(BlueprintReadOnly)
	int64 Timestamp;
};
