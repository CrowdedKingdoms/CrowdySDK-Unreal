#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CrowdyStateBlueprintLibrary.generated.h"

/**
 * Static Blueprint entry points for the CrowdyState manual-dirty push, so a property marked
 * meta=(CrowdyState, CrowdyManualDirty) can be scheduled from anywhere without first getting the actor's
 * Crowdy Entity Component by hand. Each node resolves the target actor's UCrowdyEntityComponent and forwards
 * to its member MarkStateDirty / MarkAllStateDirty (which route to the state replicator by NetID). Safe no-op
 * (never a crash) when the target is null, has no entity component, or is not driven by the state replicator
 * (a proxy, or bUseStateReplicator off).
 *
 * The Target pin defaults to self, so from an actor's own graph you wire nothing and just pick the property.
 * The dropdown on Mark Crowdy State Dirty's Property Name pin is supplied in-editor by
 * FCrowdyStatePropertyPinFactory, which lists the target actor class's CrowdyManualDirty properties.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyStateBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Schedules one CrowdyManualDirty property on the target actor's entity to ship on the next replication
	 * tick. Marking a non-manual-dirty (auto-diffed) property is a harmless no-op. Target defaults to self.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|State",
		meta = (DefaultToSelf = "Target", DisplayName = "Mark Crowdy State Dirty"))
	static void MarkCrowdyStateDirty(AActor* Target, FName PropertyName);

	/**
	 * Schedules every CrowdyManualDirty property on the target actor's entity to ship on the next replication
	 * tick. Target defaults to self.
	 */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|State",
		meta = (DefaultToSelf = "Target", DisplayName = "Mark All Crowdy States Dirty"))
	static void MarkAllCrowdyStatesDirty(AActor* Target);
};
