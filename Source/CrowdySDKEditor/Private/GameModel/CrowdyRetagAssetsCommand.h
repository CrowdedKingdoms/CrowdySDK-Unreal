// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"

/**
 * The one-time editor console command (crowdy.schema.RetagAssets) that resaves every Blueprint the Game Model
 * container scan cannot yet answer for from its asset-registry tags, so a project's negative marker reaches every
 * Blueprint in one deliberate pass instead of only as designers happen to open and save things.
 */
namespace CrowdyRetagAssetsCommand
{
	/**
	 * Orders Candidates so a parent always precedes its child, guarding against a cycle in ChildToParent rather than
	 * recursing forever. Writing a tag changes package bytes, so resaving a child before its parent would overwrite
	 * a package hash the parent's own resave immediately invalidates; this ordering is what keeps that from
	 * happening.
	 *
	 * A candidate whose recorded parent is not itself a candidate (already tag-current, or not a Blueprint at all)
	 * is treated as already resolved and never appears in the output on the parent's behalf; the child is simply
	 * free to be placed as soon as its own turn comes. A candidate absent from ChildToParent (no parent recorded) is
	 * a root and is placed the same way. A cycle is broken rather than hung on: a node reached while its own
	 * ancestor walk is still open is treated as already resolved for that walk, so every node in the cycle is still
	 * placed exactly once.
	 *
	 * Pure and static.
	 */
	TArray<FSoftObjectPath> TopoSortParentFirst(
		const TMap<FSoftObjectPath, FSoftObjectPath>& ChildToParent,
		const TArray<FSoftObjectPath>& Candidates);

	/**
	 * The object path named by one Blueprint's raw ParentClassPath asset-registry tag value, e.g.
	 * "Class'/Game/Foo.Foo_C'" resolves to "/Game/Foo.Foo_C". Empty input and the literal "None" (the tag's
	 * no-parent marker) both resolve to an empty string, terminating a parent-chain walk. A value that is already a
	 * plain object path, with no export-text wrapper, is returned unchanged.
	 *
	 * Pure and static.
	 */
	FString ResolveExportPathToObjectPath(const FString& RawExportPathOrNone);

	/** Registers (and releases) the crowdy.schema.RetagAssets console command. Called from the module lifecycle. */
	void Register();
	void Unregister();
}
