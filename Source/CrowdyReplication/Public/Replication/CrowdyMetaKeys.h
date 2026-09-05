// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

/**
 * Shared FName metadata constants stamped during Blueprint compilation and read back by the runtime router.
 *
 * They live in the runtime module rather than an editor one because both ends need them: the compile pass that
 * writes them runs wherever a Blueprint is compiled, including in an uncooked non-editor process, while
 * UCrowdyAutoRegistry reads them at runtime. Cooked builds strip UObject metadata, so a packaged runtime answers
 * these questions from UCrowdyBakedRegistry instead.
 */
namespace CrowdyMetaKeys
{
	// Universal marker for any Crowdy function. On a "Crowdy Replicates" custom event it is
	// stamped onto the generated UFunction during Blueprint compilation (see the compiler
	// extension's ApplyReplicatedMeta) so TFieldIterator/the router find it.
	CROWDYREPLICATION_API extern const FName CrowdyEvent;

	// Key-only UUserDefinedStruct metadata tags. These are inclusive flags
	// and can coexist with each other.
	CROWDYREPLICATION_API extern const FName CrowdyPersistent;
	CROWDYREPLICATION_API extern const FName CrowdySingleton;

	// Stamped onto the generated UClass during Blueprint compilation when the
	// class's component list contains a UCrowdyEntityComponent. Fast path for a
	// class that was compiled in this process: at runtime UCrowdyAutoRegistry falls
	// back to a construction-script / CDO component walk, which works in packaged builds.
	CROWDYREPLICATION_API extern const FName CrowdyEntity;
}
