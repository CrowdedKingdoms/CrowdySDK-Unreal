// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Replication/GameModel/CrowdyGameModelSessionTypes.h"
#include "CrowdyGameModel.generated.h"

class UCrowdyGameModelSubsystem;

/**
 * Blueprint access to the Game Model sessions/turns and free-container surface: the subsystem itself (so a UI can bind
 * OnDataContainerChanged), the local user id and turn-gating helpers, and typed reads of a free/data
 * container's cached properties by containerId. Actor-bound Server Owned reads are UCrowdyModel; this
 * library is for containers with no entity (inventories, quests, matches) plus the session helpers.
 * Every function resolves the subsystem off WorldContext, mirroring how UCrowdyModel resolves off an actor.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGameModel : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// The Game Model subsystem for WorldContext's world, or null outside a play world. A UI binds
	// OnDataContainerChanged on the returned object to react to free/data container changes.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Advanced", meta = (WorldContext = "WorldContext"),
		DisplayName = "Get Game Model Subsystem")
	static UCrowdyGameModelSubsystem* GetGameModelSubsystem(const UObject* WorldContext);

	// The local player's Game Model user id, or 0 when signed out.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Sessions & Turns", meta = (WorldContext = "WorldContext"),
		DisplayName = "Get Local User Id")
	static int64 GetLocalUserId(const UObject* WorldContext);

	// True when Session's current turn holder is the local user (client-side input gating; the server's
	// is_current_turn policy is the real enforcement).
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Sessions & Turns", meta = (WorldContext = "WorldContext"),
		DisplayName = "Is My Turn")
	static bool IsMyTurn(const UObject* WorldContext, const FCrowdyGameModelSession& Session);

	// Typed reads of a free/data container's cached property by key. Return Default when the container is
	// not watched, the key is not cached yet, or the cached value is a different type.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", meta = (WorldContext = "WorldContext"),
		DisplayName = "Get Model Attribute (Integer) by Id")
	static int32 GetContainerInt(const UObject* WorldContext, const FString& ContainerId, FName Key, int32 Default = 0);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", meta = (WorldContext = "WorldContext"),
		DisplayName = "Get Model Attribute (Float) by Id")
	static float GetContainerFloat(const UObject* WorldContext, const FString& ContainerId, FName Key, float Default = 0.0f);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", meta = (WorldContext = "WorldContext"),
		DisplayName = "Get Model Attribute (Boolean) by Id")
	static bool GetContainerBool(const UObject* WorldContext, const FString& ContainerId, FName Key, bool bDefault = false);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Attributes", meta = (WorldContext = "WorldContext"),
		DisplayName = "Get Model Attribute (String) by Id")
	static FString GetContainerString(const UObject* WorldContext, const FString& ContainerId, FName Key, const FString& Default = TEXT(""));

	// Typed reads of one field from a collection item's StateJson (the state fetched by "Get Collection With Items'
	// State"). Pure: parse the item's JSON state object and read Key. Return Default when the JSON is empty/invalid,
	// the key is absent, or its value is a different type. Key is the item's server property key (lowercase).
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Get Item (Integer)")
	static int32 GetItemInt(const FString& ItemStateJson, FName Key, int32 Default = 0);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Get Item (Float)")
	static float GetItemFloat(const FString& ItemStateJson, FName Key, float Default = 0.0f);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Get Item (Boolean)")
	static bool GetItemBool(const FString& ItemStateJson, FName Key, bool bDefault = false);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Get Item (String)")
	static FString GetItemString(const FString& ItemStateJson, FName Key, const FString& Default = TEXT(""));

	// The same typed reads taking a collection item struct directly (from "Get Collection With Items' State"), so a
	// Blueprint reads a field off the item without first pulling out its StateJson. Each delegates to the matching
	// StateJson getter above, so the default/mismatch behaviour is identical.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Get Item Field (Integer)")
	static int32 GetItemFieldInt(const FCrowdyCollectionItem& Item, FName Key, int32 Default = 0);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Get Item Field (Float)")
	static float GetItemFieldFloat(const FCrowdyCollectionItem& Item, FName Key, float Default = 0.0f);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Get Item Field (Boolean)")
	static bool GetItemFieldBool(const FCrowdyCollectionItem& Item, FName Key, bool bDefault = false);

	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Get Item Field (String)")
	static FString GetItemFieldString(const FCrowdyCollectionItem& Item, FName Key, const FString& Default = TEXT(""));

	// Start/stop caching + notification-driven re-pull for a free/data container without an immediate pull (a
	// UI that binds OnDataContainerChanged before the first change). Watching is also implied by a pull/create/
	// invoke on the container.
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Advanced", meta = (WorldContext = "WorldContext"),
		DisplayName = "Watch Game Model")
	static void WatchDataContainer(const UObject* WorldContext, const FString& ContainerId);

	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Advanced", meta = (WorldContext = "WorldContext"),
		DisplayName = "Unwatch Game Model")
	static void UnwatchDataContainer(const UObject* WorldContext, const FString& ContainerId);
};
