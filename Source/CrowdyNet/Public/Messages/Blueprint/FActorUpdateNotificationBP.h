#pragma once
#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "StructUtils/InstancedStruct.h"
#include "FActorUpdateNotificationBP.generated.h"

USTRUCT(BlueprintType, meta=(DisplayName="Actor Update Notification"))
struct FActorUpdateNotificationBP
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Messages")
	int64 ChunkX;
	
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Messages")
	int64 ChunkY;
	
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Messages")
	int64 ChunkZ;
	
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Messages")
	FString UUID;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Messages")
	FInstancedStruct ActorUpdate;
	
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Messages")
	int64 Timestamp;
};
