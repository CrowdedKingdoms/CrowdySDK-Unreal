// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;

/**
 * How the "Apply Crowdy Effect" nodes decide whether the Blueprint they sit in is a Game Model container, which is
 * what an unwired Target pin applies to.
 *
 * The answer lives in the Blueprint's persisted container marker, and that marker is defined in an editor-only
 * module this one must not depend on: CrowdyNodes is UncookedOnly, so an uncooked non-editor target would have no
 * editor module to link against. The editor module installs the resolver at startup instead. With no resolver set,
 * the nodes fall back to reading the container tag off the class, which is correct whenever a compile has already
 * stamped it.
 */
namespace CrowdyApplyEffectNodeShared
{
	CROWDYNODES_API void SetContainerBlueprintResolver(TFunction<bool(const UBlueprint*)> Resolver);

	// The installed resolver, so a caller that overrides it temporarily can put the original back. Returns an
	// unset function when none is installed.
	CROWDYNODES_API TFunction<bool(const UBlueprint*)> GetContainerBlueprintResolver();

	// True when the installed resolver recognizes this Blueprint as a Game Model container. False when no resolver
	// is installed, so a caller must still consult the class tag before concluding it is not one.
	CROWDYNODES_API bool IsGameModelContainerBlueprint(const UBlueprint* Blueprint);
}
