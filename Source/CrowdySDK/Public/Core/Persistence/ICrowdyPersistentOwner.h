#pragma once
#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ICrowdyPersistentOwner.generated.h"

UINTERFACE(BlueprintType, Blueprintable)
class CROWDYSDK_API UCrowdyPersistentOwner : public UInterface
{
	GENERATED_BODY()
};

/**
 * Implement on any actor or UObject whose state should be persisted via
 * UCrowdyPersistenceSubsystem.
 *
 * ── GetPersistentKey ──────────────────────────────────────────────────────────
 *   Returns the stable key used to slot this instance in the voxel store.
 *   Default: GetPathName(), which is stable for level-placed actors.
 *   Override for dynamically spawned actors to return your game's own stable ID
 *   (e.g. a faction ID, an integer slot index, a persisted GUID).
 *
 * ── OnPersistentStateRestored ─────────────────────────────────────────────────
 *   Fires on the game thread after PullState writes restored data into the
 *   output variable.  Override in Blueprint or C++ to react (refresh mesh,
 *   play sound, update UI, etc.).
 */
class CROWDYSDK_API ICrowdyPersistentOwner
{
	GENERATED_BODY()
public:

	/**
	 * Returns a stable, unique key for this object's persistent slot.
	 * Defaults to GetPathName() — always stable for level-placed actors.
	 * Override for dynamically spawned objects to return a game-assigned ID.
	 */
	UFUNCTION(BlueprintNativeEvent, Category="Crowdy|Persistence")
	FString GetPersistentKey() const;
	virtual FString GetPersistentKey_Implementation() const;

	/**
	 * Called after a PullState succeeds and the struct data has been written
	 * to the output variable.  StructType and InstanceKey identify which
	 * type/slot was restored so you can filter if needed.
	 */
	UFUNCTION(BlueprintImplementableEvent, Category="Crowdy|Persistence")
	void OnPersistentStateRestored(UScriptStruct* StructType, const FString& InstanceKey);
};
