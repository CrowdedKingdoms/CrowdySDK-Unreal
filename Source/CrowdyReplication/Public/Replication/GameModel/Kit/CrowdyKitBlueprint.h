// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * A blueprint is a self-contained declarative bundle of game-model definitions (container types, property
 * defs, policy-gated functions), optional seed containers/edges, and optional automations that implement one
 * game concept. Every element is a plain JSON object matching the corresponding server input; deploy assembles
 * them into a single seed payload with MergeBlueprints.
 */
struct CROWDYREPLICATION_API FCrowdyKitBlueprint
{
	FString Name;
	TArray<TSharedPtr<FJsonObject>> ContainerTypes;
	TArray<TSharedPtr<FJsonObject>> PropertyDefinitions;
	TArray<TSharedPtr<FJsonObject>> Functions;
	TArray<TSharedPtr<FJsonObject>> Containers;
	TArray<TSharedPtr<FJsonObject>> Edges;
	TArray<TSharedPtr<FJsonObject>> Automations;
	TArray<TSharedPtr<FJsonObject>> AutomationTriggers;
};

struct CROWDYREPLICATION_API FCrowdyMergedBlueprints
{
	TSharedPtr<FJsonObject> SeedInput;
	TArray<TSharedPtr<FJsonObject>> Automations;
	TArray<TSharedPtr<FJsonObject>> AutomationTriggers;
};

namespace CrowdyKit
{
	// Read a string member of a JSON object (empty when absent or non-string).
	CROWDYREPLICATION_API FString BlueprintField(const TSharedPtr<FJsonObject>& Object, const FString& Key);

	/**
	 * Merge blueprints into one seed payload plus the automation upserts, rejecting duplicate
	 * type/property/function/container/automation names across blueprints. On a collision the merge stops,
	 * appends a human-readable message (naming the kind: "container type", "property", "function",
	 * "container tempId", "automation") to OutErrors, and returns false. Automations and their triggers get the
	 * app id bound as a string.
	 */
	CROWDYREPLICATION_API bool MergeBlueprints(const FString& AppId, const TArray<FCrowdyKitBlueprint>& Blueprints,
		const FString& SessionId, FCrowdyMergedBlueprints& Out, TArray<FString>& OutErrors);

	// Concatenate several blueprints into ONE composite blueprint (no collision checks, which happen at deploy).
	CROWDYREPLICATION_API FCrowdyKitBlueprint ComposeBlueprints(const FString& Name,
		const TArray<FCrowdyKitBlueprint>& List);
}
