#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CrowdyReplicatedEventLibrary.generated.h"

/**
 * Internal Blueprint surface for the RPC-style CrowdyEvent system. The compiler extension
 * splices a call to CrowdyDispatchReplicatedEvent into the head of every event flagged
 * "Crowdy Replicates"; the node is marked BlueprintInternalUseOnly and is not meant to be
 * placed by hand.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyReplicatedEventLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Gate placed at the head of a "Crowdy Replicates" event. As a CustomThunk it runs in the
	 * caller's stack frame, so it reads the marked event's own function and live parameters
	 * directly and either routes the call over the Crowdy transport or recognises this
	 * invocation as the local replay of a received call. Returns true when the call was routed
	 * and the event body must be skipped, false when the body should run.
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Crowdy SDK|Events",
		meta = (BlueprintInternalUseOnly = "true"))
	static bool CrowdyDispatchReplicatedEvent();
	DECLARE_FUNCTION(execCrowdyDispatchReplicatedEvent);
};
