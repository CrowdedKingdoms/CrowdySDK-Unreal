// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Replication/GameModel/CrowdyGameModelSessionTypes.h"       // FCrowdyContainerRef, FCrowdyCollectionItem
#include "Replication/GameModel/CrowdyGameModelContainerActions.h"   // reuse the container/graph outcome delegates
#include "CrowdyGameModelCollectionActions.generated.h"

// One item set carrying each item's fetched state (Get Collection With Items' State).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdyCollectionItemsOutcome, TArray<FCrowdyCollectionItem>, Items);

/**
 * Model Collection nodes: the designer-facing surface for a container that owns other containers (an inventory
 * of items, a party of members). A collection is the set of edges of one relationship type from a parent
 * container to item containers; the SDK provisions the reserved crowdy_rev counter + touch function (schema sync)
 * so a membership change still notifies watchers. Add/Remove need the parent's container type so they can invoke
 * its per-type touch function after the edge changes; CollectionName defaults to "contains".
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyAddToCollectionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Collections")
	FCrowdyPullDataContainerOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Collections")
	FCrowdyPullDataContainerOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Add To Collection")
	static UCrowdyAddToCollectionAction* AddToCollection(UObject* WorldContext, const FString& ParentContainerId,
		const FString& ParentContainerType, const FString& ItemContainerId, const FString& CollectionName = TEXT("contains"));

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString ParentContainerId;
	FString ParentContainerType;
	FString ItemContainerId;
	FString CollectionName;
};

/** Unlinks an item from a collection (resolves + deletes the parent -> item edge) and notifies watchers. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyRemoveFromCollectionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Collections")
	FCrowdyPullDataContainerOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Collections")
	FCrowdyPullDataContainerOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Remove From Collection")
	static UCrowdyRemoveFromCollectionAction* RemoveFromCollection(UObject* WorldContext, const FString& ParentContainerId,
		const FString& ParentContainerType, const FString& ItemContainerId, const FString& CollectionName = TEXT("contains"));

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString ParentContainerId;
	FString ParentContainerType;
	FString ItemContainerId;
	FString CollectionName;
};

/** Lists a collection's item containers (the depth-1 members of the parent). No state is fetched; read a member's
 *  properties with the Get Container getters after a pull, or use Get Collection With Items' State. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGetCollectionAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Collections")
	FCrowdyContainerRefsOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Collections")
	FCrowdyContainerRefsOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Get Collection")
	static UCrowdyGetCollectionAction* GetCollection(UObject* WorldContext, const FString& ParentContainerId,
		const FString& CollectionName = TEXT("contains"));

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString ParentContainerId;
	FString CollectionName;
};

/** Lists a collection's items AND fetches each item's visible state in one call (bounded to MaxItems, default 64).
 *  Read a scalar out of each item's StateJson with UCrowdyGameModel::GetItemInt/Float/Bool/String. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGetCollectionWithStateAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Collections")
	FCrowdyCollectionItemsOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Collections")
	FCrowdyCollectionItemsOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Get Collection With Items' State")
	static UCrowdyGetCollectionWithStateAction* GetCollectionWithState(UObject* WorldContext, const FString& ParentContainerId,
		const FString& CollectionName = TEXT("contains"), int32 MaxItems = 64);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString ParentContainerId;
	FString CollectionName;
	int32 MaxItems = 64;
};

/** Creates a container to use as a collection item (the collection-workflow name for creating a data container of
 *  an item type; the server pins ownership to the caller). Add it to a collection with Add To Collection. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyCreateModelItemAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Collections")
	FCrowdyContainerIdOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Collections")
	FCrowdyContainerIdOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Collections", DisplayName = "Create Model Item")
	static UCrowdyCreateModelItemAction* CreateModelItem(UObject* WorldContext, const FString& TypeName,
		const FString& DisplayName = FString(), const FString& SessionId = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypeName;
	FString DisplayName;
	FString SessionId;
};
