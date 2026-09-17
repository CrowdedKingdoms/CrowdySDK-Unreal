#pragma once

#include "CoreMinimal.h"
#include "Data/CrowdyContainerManifest.h"

class AActor;
class UWorld;

/** A placed actor the scan looked at and left out, with the reason a designer can act on. */
struct CROWDYSTUDIO_API FCrowdyContainerManifestSkip
{
	FString Actor;
	FString Reason;
};

struct CROWDYSTUDIO_API FCrowdyContainerManifestScanResult
{
	TArray<FCrowdyContainerManifestRow> Rows;
	TArray<FCrowdyContainerManifestSkip> Skips;
	/** Actors carrying an entity component that were examined, loaded or unloaded. */
	int32 EntityActors = 0;
};

/**
 * Walks a level in the editor and derives the Game Model containers its placed entities will bind at runtime,
 * with the same identity the runtime derives: the engine placement guid (or an authored binding key) for the
 * actor, and the RegisterSubParticipant seed for each component container. World Partition actors are reached
 * through the actor descs and loaded in batches, because a placement can override the entity settings and a desc
 * cannot say so. Per-player, random-identity and runtime-added containers are reported as skips, never guessed.
 */
class CROWDYSTUDIO_API FCrowdyContainerManifestScan
{
public:
	/** Scans every level-placed entity actor of World, loaded ones directly and World Partition ones through their descs. */
	static void ScanWorld(UWorld* World, FCrowdyContainerManifestScanResult& Out);

	/** Derives the rows of one placed actor. Public so a test can drive it with a constructed actor. */
	static void ScanActor(const AActor* Actor, FCrowdyContainerManifestScanResult& Out);

	/** Where the manifest of a map lives: beside the map, named after it. */
	static FString ManifestPackageNameForMap(const FString& MapPackageName);

	/**
	 * Writes Result into the map's manifest asset, creating or replacing it, and saves the package. An asset that
	 * already holds the same rows is left untouched (bOutWritten false), so a scan of an unchanged level makes no
	 * file change for version control to pick up. Null on failure, or for a play world. PackageNameOverride names
	 * another destination (a test's scratch package).
	 */
	static UCrowdyContainerManifest* SaveManifest(UWorld* World, const FCrowdyContainerManifestScanResult& Result,
		const FString& PackageNameOverride = FString(), bool* bOutWritten = nullptr);

	/** Whether two row sets name the same containers, whatever order the scan found them in. */
	static bool RowsMatch(const TArray<FCrowdyContainerManifestRow>& A, const TArray<FCrowdyContainerManifestRow>& B);
};
