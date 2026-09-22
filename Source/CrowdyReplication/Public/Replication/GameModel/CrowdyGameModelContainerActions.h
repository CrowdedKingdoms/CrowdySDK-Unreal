// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Replication/GameModel/CrowdyGameModelSessionTypes.h"
#include "CrowdyGameModelContainerActions.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdyContainerIdOutcome, FString, ContainerId);
// PullDataContainer's façade is bOk-only; there is no payload to carry.
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FCrowdyPullDataContainerOutcome);
// Succeeded fires only on a COMMITTED invoke (reached the server AND passed its logic/authority checks). A
// transport failure OR a server-side rollback both route to Failed, which still carries bSuccess + ErrorMessage,
// so a rolled-back invoke (a logic failure, not a network failure) is distinguishable without the "Succeeded"
// pin ever firing on a rejected mutation.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdyInvokeOnContainerOutcome, bool, bSuccess, FString, ReturnValueJson, FString, ErrorMessage);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdyEdgeOutcome, FCrowdyContainerEdge, Edge);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyTraverseOutcome, const TArray<FCrowdyContainerRef>&, Nodes, const TArray<FCrowdyContainerEdge>&, Edges);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdyContainerRefsOutcome, const TArray<FCrowdyContainerRef>&, Children);

/** Creates a free/data container (no actor) of TypeName; the server pins ownership to the caller. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyCreateDataContainerAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyContainerIdOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyContainerIdOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Create Game Model")
	static UCrowdyCreateDataContainerAction* CreateDataContainer(UObject* WorldContext, const FString& TypeName,
		const FString& DisplayName = FString(), const FString& SessionId = FString(), const FString& MetadataJson = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypeName;
	FString DisplayName;
	FString SessionId;
	FString MetadataJson;
};

/** Pulls a free/data container's visible state by id into the cache and starts watching it for change notifications. */
UCLASS()
class CROWDYREPLICATION_API UCrowdyPullDataContainerAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyPullDataContainerOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyPullDataContainerOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Refresh Game Model (by Id)")
	static UCrowdyPullDataContainerAction* PullDataContainer(UObject* WorldContext, const FString& ContainerId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString ContainerId;
};

/**
 * Invokes a function against a free/data container by id (not an actor). ParamsJson, when non-empty, must be
 * a JSON object literal; a malformed literal fails the node immediately without reaching the server.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyInvokeOnContainerAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyInvokeOnContainerOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyInvokeOnContainerOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Call Model Function (by Id, Raw JSON)")
	static UCrowdyInvokeOnContainerAction* InvokeOnContainer(UObject* WorldContext, const FString& ContainerId,
		const FString& FunctionName, const FString& ParamsJson = FString(), const FString& SessionId = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString ContainerId;
	FString FunctionName;
	FString ParamsJson;
	FString SessionId;
};

/** Adds a directed relationship edge between two containers (inventory -> item, chest -> contents, tech-tree link). */
UCLASS()
class CROWDYREPLICATION_API UCrowdyAddEdgeAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyEdgeOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyEdgeOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Link Models")
	static UCrowdyAddEdgeAction* AddEdge(UObject* WorldContext, const FString& FromContainerId,
		const FString& ToContainerId, const FString& RelationshipType, float Weight = 0.0f,
		const FString& MetadataJson = FString());

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString FromContainerId;
	FString ToContainerId;
	FString RelationshipType;
	float Weight = 0.0f;
	bool bHasWeight = false;
	FString MetadataJson;
};

/** Walks the container graph from RootId up to Depth hops (clamped to 5 server-side). */
UCLASS()
class CROWDYREPLICATION_API UCrowdyTraverseAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyTraverseOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyTraverseOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Find Linked Models")
	static UCrowdyTraverseAction* Traverse(UObject* WorldContext, const FString& RootId,
		const FString& RelationshipType, int32 Depth = 1);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString RootId;
	FString RelationshipType;
	int32 Depth = 1;
};

/** The depth-1 node list from RootId (an inventory's items, a chest's loot). */
UCLASS()
class CROWDYREPLICATION_API UCrowdyListChildrenAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyContainerRefsOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyContainerRefsOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Get Linked Models")
	static UCrowdyListChildrenAction* ListChildren(UObject* WorldContext, const FString& RootId, const FString& RelationshipType);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString RootId;
	FString RelationshipType;
};

/**
 * Directly writes one property on a free/data container (owner/admin-writable props only). ValueJson is a
 * JSON-encoded value literal ("\"Aria\"", "42", "true"); the server coerces it to the property's declared type.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdySetDataPropertyAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyPullDataContainerOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyPullDataContainerOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Set Model Attribute (by Id)")
	static UCrowdySetDataPropertyAction* SetDataProperty(UObject* WorldContext, const FString& ContainerId,
		const FString& Key, const FString& ValueType, const FString& ValueJson);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString ContainerId;
	FString Key;
	FString ValueType;
	FString ValueJson;
};

/**
 * Deletes a free/data (or actor-bound) container instance by id (owner-or-admin, server-enforced). The server
 * cascades the container's properties + every connected edge; on Success the subsystem drops all local state for
 * the id and broadcasts OnDataContainerChanged. Failed carries no payload (Succeeded vs Failed is the outcome).
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyDeleteContainerAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyPullDataContainerOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyPullDataContainerOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Delete Game Model")
	static UCrowdyDeleteContainerAction* DeleteContainer(UObject* WorldContext, const FString& ContainerId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString ContainerId;
};

/** Deletes one directed graph edge by id (source-container owner or admin, server-enforced). */
UCLASS()
class CROWDYREPLICATION_API UCrowdyDeleteEdgeAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyPullDataContainerOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced")
	FCrowdyPullDataContainerOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Advanced", DisplayName = "Unlink Models")
	static UCrowdyDeleteEdgeAction* DeleteEdge(UObject* WorldContext, const FString& EdgeId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString EdgeId;
};
