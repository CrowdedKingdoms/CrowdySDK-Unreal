#pragma once
#include "CoreMinimal.h"
#include "Replication/ObjectHandler/CrowdyEntityEventHandler.h"
#include "FCrowdyObjectManagerConfig.generated.h"

USTRUCT(BlueprintType)
struct FCrowdyObjectHandlerBinding
{
	GENERATED_BODY()

	/**
	 * Must match an EventName in your EventPayloadType data asset.
	 * Validated at edit time — see PostEditChangeProperty in dev settings.
	 */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Object Management")
	FName EventName;

	// Class ref instead of instanced object — survives config serialization
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Object Management")
	TSoftClassPtr<UCrowdyEntityEventHandler> HandlerClass;
};

USTRUCT(BlueprintType)
struct FCrowdyObjectManagerConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Object Management")
	TArray<FCrowdyObjectHandlerBinding> HandlerBindings;
};