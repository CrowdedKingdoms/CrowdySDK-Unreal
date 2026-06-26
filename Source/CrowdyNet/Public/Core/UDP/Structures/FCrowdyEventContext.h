#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "FCrowdyEventContext.generated.h"

/**
 * Envelope metadata for an inbound game event. Handlers receive it by declaring
 * it as an optional second parameter:
 *
 *   void Handler(const FMyEvent& Event, const FCrowdyEventContext& Context)
 */
USTRUCT(BlueprintType)
struct FCrowdyEventContext
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Events")
	FGuid SenderID;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Events")
	FGuid TargetID;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Events")
	ECrowdyTarget Target = ECrowdyTarget::Everyone;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Events")
	bool bSentByLocalPlayer = false;
};
