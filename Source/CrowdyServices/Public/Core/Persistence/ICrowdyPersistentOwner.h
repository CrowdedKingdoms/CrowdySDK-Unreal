#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ICrowdyPersistentOwner.generated.h"

UINTERFACE(BlueprintType, Blueprintable)
class CROWDYSERVICES_API UCrowdyPersistentOwner : public UInterface
{
	GENERATED_BODY()
};

/**
 * Implement on any actor or UObject whose state should be persisted via
 * UCrowdyPersistenceSubsystem.
 *
 * GetPersistentKey returns the stable key that slots this instance in the voxel store.
 * Override it for dynamically spawned actors to return your game's own stable ID.
 * OnPersistentStateRestored fires on the game thread after PullState writes restored data into the output variable.
 */
class CROWDYSERVICES_API ICrowdyPersistentOwner
{
	GENERATED_BODY()
public:

	/**
	 * Returns a stable, unique key for this object's persistent slot.
	 * Defaults to the object's path with any PIE prefix removed, which is stable for level-placed actors.
	 * Override for dynamically spawned objects to return a game-assigned ID.
	 */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy SDK|Persistence")
	FString GetPersistentKey() const;
	virtual FString GetPersistentKey_Implementation() const;

	/**
	 * Called after a PullState succeeds and the struct data has been written
	 * to the output variable.  StructType and InstanceKey identify which
	 * type/slot was restored so you can filter if needed.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category="Crowdy SDK|Persistence")
	void OnPersistentStateRestored(UScriptStruct* StructType, const FString& InstanceKey);
};
