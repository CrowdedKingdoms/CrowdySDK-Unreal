#pragma once

#include "CoreMinimal.h"

class FArchive;
class UScriptStruct;

/**
 * One contiguous span of a struct's memory that a binary archive moves verbatim, in the order the
 * archive visits it. Adjacent spans merge, so a fully packed struct becomes a single copy.
 */
struct FCrowdyCopyRun
{
	int32 Offset = 0;
	int32 Size = 0;

	// A bool reaches an archive as 0 or 1 rather than as whatever byte it holds, so its span is
	// normalised on the way past instead of copied. It never merges with a neighbour.
	bool bNormalisedBool = false;
};

/**
 * A struct's reflective property walk reduced to a handful of memory copies.
 *
 * Built only for a struct whose bytes are proven identical either way, over several byte patterns and
 * in both directions, so a plan can never put a byte on the wire the property walk would not have.
 */
struct FCrowdyCopyPlan
{
	TArray<FCrowdyCopyRun> Runs;
	int32 TotalBytes = 0;

	bool IsValid() const { return Runs.Num() > 0 && TotalBytes > 0; }
};

namespace CrowdyPodCopyPlan
{
	/** Builds a plan, or returns an invalid one and says why. Callers on a message path want Find. */
	CROWDYNET_API FCrowdyCopyPlan Build(const UScriptStruct* Struct, FString& OutReason);

	/**
	 * The cached plan for a struct, or null when the struct has to keep the reflective walk. A struct is
	 * walked at most once: a refusal is remembered too, so a refused struct is never walked again.
	 */
	CROWDYNET_API const FCrowdyCopyPlan* Find(const UScriptStruct* Struct);

	/**
	 * Moves the plan's bytes between the struct and the archive, in whichever direction the archive runs.
	 *
	 * False means this archive is not the shape the plan was proven against, or too few bytes remain to
	 * fill the struct, and the caller has to fall back to the reflective walk. Both are decided before
	 * anything is read or written, so that false costs the caller only the check.
	 *
	 * False also means the archive failed while the plan was running, which is the one false that arrives
	 * with bytes already moved. Consult the archive rather than repeating the call: a true is only ever
	 * given for an archive that is still good.
	 */
	CROWDYNET_API bool Apply(const FCrowdyCopyPlan& Plan, FArchive& Ar, void* Container);

	/** Drops every cached plan. A plan is a table of byte offsets, so it dies with the reflection it came from. */
	CROWDYNET_API void InvalidateAll();

#if WITH_DEV_AUTOMATION_TESTS
	/** Plans built since the last InvalidateAll. A plan built per message rather than per struct shows here. */
	CROWDYNET_API int32 GetBuildCount();

	/** Replaces a struct's cached plan, so a test can tell a plan that ran from one that was ignored. */
	CROWDYNET_API void InstallForTests(const UScriptStruct* Struct, FCrowdyCopyPlan Plan);
#endif
}
