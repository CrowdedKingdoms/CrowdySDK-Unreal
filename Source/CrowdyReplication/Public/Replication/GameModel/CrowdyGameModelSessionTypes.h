// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "CrowdyGameModelSessionTypes.generated.h"

/**
 * Blueprint-facing mirrors of the Game Model Tier B wire types (sessions, graph edges, free/data containers).
 * FCrowdyGameApiCodec (CrowdyNet) parses the GraphQL envelopes into its own plain
 * structs; UCrowdyGameModelSubsystem maps those into these BlueprintType structs at the façade boundary so
 * gameplay Blueprints see typed data, never raw JSON. BigInt user ids are surfaced as int64 (Blueprint's
 * Integer64); a user id is never 0 on the server, so 0 reads as "unset" for the optional turn holder, with an
 * explicit bHasCurrentTurn for callers that would rather not assume that.
 */

/** One runtime session (a match, battle, or room scope) that containers can be attached to. */
USTRUCT(BlueprintType)
struct FCrowdyGameModelSession
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString Status;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 CreatedByUserId = 0;

	// The user whose turn it currently is, or 0 when no turn is set (see bHasCurrentTurn). Read is_current_turn
	// enforcement happens server-side; this is the client-side mirror a game gates input on.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 CurrentTurnUserId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	bool bHasCurrentTurn = false;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString MetadataJson;
};

/** A directed relationship edge between two containers (inventory -> item, chest -> contents, tech-tree link). */
USTRUCT(BlueprintType)
struct FCrowdyContainerEdge
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString EdgeId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString FromContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString ToContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString RelationshipType;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	float Weight = 0.0f;
};

/**
 * A free/data container reference (a container with no actor: an inventory, an item, a quest, a match). Carries
 * the identity + ownership metadata a UI needs; its property values are read separately by containerId through
 * the subsystem's per-container cache (UCrowdyGameModel::GetContainerInt/Float/... ).
 */
USTRUCT(BlueprintType)
struct FCrowdyContainerRef
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString ContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString TypeName;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString DisplayName;

	// The owning user, or 0 when the container is unowned.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	int64 OwnerUserId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString SessionId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString MetadataJson;
};

/**
 * One item of a Model Collection with its visible state fetched in the same call ("Get Collection With Items'
 * State"). ContainerId + TypeName identify the item; StateJson is its visible property set as a JSON object
 * string (read a scalar out of it with UCrowdyGameModel::GetItemInt/Float/Bool/String, or parse it for a rich
 * field). Unlike a plain Get Collection this carries the state, so a UI shows a whole bag in one round-trip.
 */
USTRUCT(BlueprintType)
struct FCrowdyCollectionItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString ContainerId;

	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString TypeName;

	// The item's visible properties as a JSON object string ({"quantity":5,"item_id":"sword"}); empty if the
	// item's state could not be read. Read a scalar with UCrowdyGameModel::GetItem* helpers.
	UPROPERTY(BlueprintReadOnly, Category = "Crowdy SDK|Game Model")
	FString StateJson;
};
