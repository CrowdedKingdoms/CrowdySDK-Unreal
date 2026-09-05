// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "CrowdyWorldsimKitActions.generated.h"

class FJsonObject;
class UCrowdyGameModelSubsystem;

// A parsed, read-only view of the world clock/weather singleton, returned by Get World State.
USTRUCT(BlueprintType)
struct FCrowdyWorldState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FString ContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	int32 TimeOfDay = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	int32 Day = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FString Weather;
};

// A parsed, read-only view of one regenerating resource node, returned by List Resource Nodes.
USTRUCT(BlueprintType)
struct FCrowdyResourceNode
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FString ContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FString DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FString NodeId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FString ResourceItemId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	int32 Amount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	int32 MaxAmount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	int32 RegenRate = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	float X = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	float Y = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	float Z = 0.f;
};

// A parsed, read-only view of one crop / production job, returned by List Crops. bReady is true once the crop has
// matured (max_stage > 0 and stage has reached max_stage), so a caller can decide to harvest without recomputing.
USTRUCT(BlueprintType)
struct FCrowdyCrop
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FString ContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FString DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FString OwnerUserId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	int32 Stage = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	int32 MaxStage = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FString OutputItemId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	int32 OutputQty = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	bool bReady = false;
};

// On success State holds the read clock/weather and ErrorMessage is empty; on failure State is defaulted and
// ErrorMessage explains why (no WorldState exists yet, transport failure, a bad read).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyGetWorldStateOutcome, const FCrowdyWorldState&, State, const FString&, ErrorMessage);

// On success Nodes holds the parsed resource nodes (possibly empty) and ErrorMessage is empty; on failure Nodes is
// empty and ErrorMessage explains why.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyListResourceNodesOutcome, const TArray<FCrowdyResourceNode>&, Nodes, const FString&, ErrorMessage);

// On success ReturnValueJson carries the gather function's server return (the node's remaining amount) and
// ErrorMessage is empty; on failure ErrorMessage carries the server envelope's reason (a policy denial reads back
// verbatim).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyGatherNodeOutcome, const FString&, ReturnValueJson, const FString&, ErrorMessage);

// On success ContainerId names the new crop and ErrorMessage is empty; on failure ContainerId is empty and
// ErrorMessage explains why (not signed in, transport/server refusal, a rejected property write).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyPlantCropOutcome, const FString&, ContainerId, const FString&, ErrorMessage);

// On success Crops holds the parsed crops (possibly empty) and ErrorMessage is empty; on failure Crops is empty
// and ErrorMessage explains why.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyListCropsOutcome, const TArray<FCrowdyCrop>&, Crops, const FString&, ErrorMessage);

// On success ReturnValueJson carries the harvest function's server return (the yield quantity) and ErrorMessage is
// empty; on failure ErrorMessage carries the server envelope's reason.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FCrowdyHarvestCropOutcome, const FString&, ReturnValueJson, const FString&, ErrorMessage);

/**
 * Read the world clock/weather singleton (time_of_day / day / weather) for a deployed Worldsim kit. TypePrefix
 * selects the deployed type name (<Prefix>WorldState); leave it empty for a kit deployed with no prefix. The
 * WorldState singleton is admin-created (there is no player-facing node to make one), so if none exists yet Failed
 * carries a clear reason.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGetWorldStateAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyGetWorldStateOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyGetWorldStateOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Worldsim", DisplayName = "Get World State")
	static UCrowdyGetWorldStateAction* GetWorldState(UObject* WorldContext, const FString& TypePrefix,
		const FString& SessionId = FString());

	// The pure parser: read the clock/weather out of a pulled container-state property map. ContainerId is
	// threaded onto the returned struct. Public + static so it is headless-testable with no world/subsystem/HTTP.
	static FCrowdyWorldState ParseWorldState(const TSharedPtr<FJsonObject>& State, const FString& ContainerId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypePrefix;
	FString SessionId;
};

/**
 * List the deployed Worldsim kit's regenerating resource nodes with parsed state (amount / max_amount / regen_rate
 * / position). TypePrefix selects the deployed type name (<Prefix>ResourceNode). Players gather from a node with
 * Gather Node.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyListResourceNodesAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyListResourceNodesOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyListResourceNodesOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Worldsim", DisplayName = "List Resource Nodes")
	static UCrowdyListResourceNodesAction* ListResourceNodes(UObject* WorldContext, const FString& TypePrefix,
		const FString& SessionId = FString());

	// The pure parser: read one node's state out of a pulled property map. ContainerId + DisplayName come from the
	// list row (they are container metadata, not property state). Public + static so it is headless-testable.
	static FCrowdyResourceNode ParseResourceNode(const TSharedPtr<FJsonObject>& State, const FString& ContainerId,
		const FString& DisplayName);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypePrefix;
	FString SessionId;
};

/**
 * Gather from a shared resource node into a caller-owned stack of the node's resource item. The node decrement and
 * the grant commit atomically server-side (no over-gathering races); this node only names the node, the amount,
 * and the receiving stack. NodeContainerId is a resource-node container id (from List Resource Nodes);
 * ToStackContainerId is the caller-owned stack container that receives the units. On success ReturnValueJson is
 * the node's remaining amount. A policy denial (not enough units, wrong item, not your stack) reads back verbatim
 * on Failed.
 *
 * TypePrefix must match the prefix the kit was deployed under, so the correct gather function is called.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyGatherNodeAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyGatherNodeOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyGatherNodeOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Worldsim", DisplayName = "Gather Node")
	static UCrowdyGatherNodeAction* GatherNode(UObject* WorldContext, const FString& TypePrefix,
		const FString& NodeContainerId, int32 Amount, const FString& ToStackContainerId,
		const FString& SessionId = FString());

	// The pure param marshaller: amount is a JSON number, to_stack_id is a container_ref which on the wire is the
	// bare stack container-id STRING. Public + static so it is headless-testable with no world/subsystem/HTTP.
	static TSharedPtr<FJsonObject> BuildGatherParams(int32 Amount, const FString& ToStackId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypePrefix;
	FString NodeContainerId;
	int32 Amount = 0;
	FString ToStackContainerId;
	FString SessionId;
};

/**
 * Plant a crop / start a production job: create a <Prefix>Crop container and seed its output item, quantity, and
 * maturity target. The crop matures server-side via the kit's growth automation; harvest it with Harvest Crop once
 * it is ready. owner_user_id mirrors the local Game Model user id when signed in (best-effort; the server already
 * pins the record owner on create). On success ContainerId names the new crop.
 *
 * TypePrefix selects the deployed type name (<Prefix>Crop). DisplayName defaults to "Crop <OutputItemId>".
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyPlantCropAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyPlantCropOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyPlantCropOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Worldsim", DisplayName = "Plant Crop")
	static UCrowdyPlantCropAction* PlantCrop(UObject* WorldContext, const FString& TypePrefix,
		const FString& OutputItemId, int32 OutputQty = 1, int32 MaxStage = 3, const FString& DisplayName = FString(),
		const FString& SessionId = FString());

	virtual void Activate() override;

private:
	// Queues the property writes applied after create. bFatal writes fail the plant on rejection; the
	// owner_user_id mirror is non-fatal (the record owner is already pinned server-side).
	void BuildPendingWrites();
	// Applies the next queued write, then broadcasts Succeeded once the queue drains.
	void ApplyNextProperty();

	struct FPendingWrite
	{
		FString Key;
		FString ValueType;
		FString ValueJson;
		bool bFatal = false;
	};

	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypePrefix;
	FString OutputItemId;
	int32 OutputQty = 1;
	int32 MaxStage = 3;
	FString DisplayName;
	FString SessionId;

	TWeakObjectPtr<UCrowdyGameModelSubsystem> Model;
	FString CreatedContainerId;
	TArray<FPendingWrite> PendingWrites;
	int32 WriteIndex = 0;
};

/**
 * List crops / production jobs with parsed state. When bOnlyMine is set, only crops owned by the local user are
 * returned (a crop's owner_user_id mirror is compared against the local Game Model user id). TypePrefix selects
 * the deployed type name (<Prefix>Crop).
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyListCropsAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyListCropsOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyListCropsOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Worldsim", DisplayName = "List Crops")
	static UCrowdyListCropsAction* ListCrops(UObject* WorldContext, const FString& TypePrefix,
		bool bOnlyMine = true, const FString& SessionId = FString());

	// The pure parser: read one crop's state out of a pulled property map. ContainerId + DisplayName + OwnerUserId
	// come from the list row (container metadata, not property state); OwnerUserId is the row's ownerUserId as a
	// string. bReady is computed. Public + static so it is headless-testable.
	static FCrowdyCrop ParseCrop(const TSharedPtr<FJsonObject>& State, const FString& ContainerId,
		const FString& DisplayName, const FString& OwnerUserId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypePrefix;
	bool bOnlyMine = true;
	FString SessionId;
};

/**
 * Harvest a grown crop into a caller-owned stack of the crop's output item. The stage reset and the yield grant
 * commit atomically server-side; the crop regrows via the automation. CropContainerId is a crop container id (from
 * List Crops); ToStackContainerId is the caller-owned stack that receives the yield. On success ReturnValueJson is
 * the yield quantity. A policy denial (not grown yet, wrong item, not your crop/stack) reads back verbatim on
 * Failed.
 *
 * TypePrefix must match the prefix the kit was deployed under, so the correct harvest function is called.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyHarvestCropAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyHarvestCropOutcome Succeeded;

	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Kits|Worldsim")
	FCrowdyHarvestCropOutcome Failed;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContext"),
		Category = "Crowdy SDK|Game Model|Kits|Worldsim", DisplayName = "Harvest Crop")
	static UCrowdyHarvestCropAction* HarvestCrop(UObject* WorldContext, const FString& TypePrefix,
		const FString& CropContainerId, const FString& ToStackContainerId, const FString& SessionId = FString());

	// The pure param marshaller: to_stack_id is a container_ref which on the wire is the bare stack container-id
	// STRING. Public + static so it is headless-testable with no world/subsystem/HTTP.
	static TSharedPtr<FJsonObject> BuildHarvestParams(const FString& ToStackId);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UObject> WorldContextObject;
	FString TypePrefix;
	FString CropContainerId;
	FString ToStackContainerId;
	FString SessionId;
};
