// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/Kit/CrowdyKitBlueprint.h"

/**
 * Inventory preset: a server-authoritative per-player bag. An Inventory container plus ItemStack containers
 * (item_id, quantity, slot) and owner-gated functions to grant, consume, move, and transfer stacks. The
 * quantity guards live in the invoke policies, so an untrusted client can never overdraw or touch another
 * player's items. This slice builds the blueprint half; the runtime kit rides a later slice.
 */
struct CROWDYREPLICATION_API FCrowdyKitInventoryNames
{
	FString InventoryType;
	FString StackType;
	FString GrantFn;
	FString ConsumeFn;
	FString MoveFn;
	FString TransferFn;
	FString ContainsEdge;
};

struct CROWDYREPLICATION_API FCrowdyInventoryBlueprintOptions
{
	// Prefix for type names ("Bank" -> BankInventory/BankItemStack) and, snake-cased, for function names
	// (bank_grant_stack, ...). Lets several inventory systems coexist.
	FString TypePrefix;

	// Default max_slots on new inventories.
	int32 MaxSlots = 24;

	// Exclusive upper bound for stack slot indexes.
	int32 SlotCount = 64;
};

namespace CrowdyKit
{
	CROWDYREPLICATION_API FCrowdyKitInventoryNames InventoryNames(const FString& TypePrefix = FString());

	CROWDYREPLICATION_API FCrowdyKitBlueprint InventoryBlueprint(
		const FCrowdyInventoryBlueprintOptions& Options = FCrowdyInventoryBlueprintOptions());
}
