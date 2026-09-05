// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/SoftObjectPath.h"

#include "CrowdyContainerScan.generated.h"

/**
 * Which content roots the Game Model schema scan sweeps for container Blueprints.
 *
 * Leave Scanned Content Roots empty for the default, which is the game's own content plus the content of every
 * plugin that could declare Crowdy metadata: the CrowdySDK plugin itself, anything whose plugin dependencies reach
 * it, and any plugin installed into the project. Filling it in replaces that default outright.
 *
 * Excluded Content Roots is subtracted from whichever of the two applies, and is the setting to reach for when a
 * third-party plugin ships a large content library that cannot contain a Game Model container. Excluding a root
 * means the scan never sees anything in it, so a container placed there is invisible to the schema, which on a
 * sync makes its server-side model look like something nobody declared.
 *
 * Both are mount paths ("/Game", "/CrowdySDK"). A root matches itself and everything under it, on whole path
 * segments, and without regard to case since these are typed by hand.
 */
UCLASS(Config = Editor, DefaultConfig, meta = (DisplayName = "Crowdy Game Model Schema Scan"))
class CROWDYSTUDIO_API UCrowdySchemaScanSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Crowdy Game Model Schema Scan"); }

	UPROPERTY(Config, EditAnywhere, Category = "Game Model",
		meta = (DisplayName = "Scanned content roots"))
	TArray<FString> ScannedContentRoots;

	UPROPERTY(Config, EditAnywhere, Category = "Game Model",
		meta = (DisplayName = "Excluded content roots"))
	TArray<FString> ExcludedContentRoots;
};

/**
 * One Blueprint-family asset as the schema scan sees it from the asset registry alone, before anything is loaded.
 * ScanVersion and ContainerTypeName are the raw tag values, empty when the asset carries no such tag.
 */
struct FCrowdyScannedAsset
{
	FSoftObjectPath ObjectPath;
	FString AssetPath;
	FString ScanVersion;
	FString ContainerTypeName;
};

/**
 * How one scan partitioned the project's Blueprint assets.
 *
 * AssetsToLoad decides what the schema plan itself loads, and it is deliberately NOT "the assets the tags said
 * were containers". It is the containers the tags named PLUS every asset the tags could not answer for, because an
 * untagged container has to stay visible: a container that vanishes from a schema plan turns every model the live
 * app already holds into a prune candidate. UnmigratedAssets decides the separate question of what the retag
 * migration resaves. The counts are for the status line and the log, so a project still falling back to loading
 * says so out loud rather than being quietly slow.
 */
struct CROWDYSTUDIO_API FCrowdyContainerScanPlan
{
	TArray<FSoftObjectPath> AssetsToLoad;

	// The subset of AssetsToLoad the tags could not answer for at all: no scan tag, or an older one. This is the
	// migration's real candidate set, deliberately NOT the same list as AssetsToLoad, which also carries every
	// already-tagged known container. Retagging one of those would gain nothing and cost a resave for free.
	TArray<FSoftObjectPath> UnmigratedAssets;

	// The container type names the registry supplied without a load, sorted. Reported, never used as the candidate
	// set: the classes themselves still come from reflection after the load.
	TArray<FString> KnownContainerTypeNames;

	int32 SweptAssetCount = 0;
	int32 KnownContainerCount = 0;
	int32 KnownNonContainerCount = 0;
	int32 UnmigratedAssetCount = 0;

	// The one-line record of what the scan avoided and what it still has to load.
	FString Describe() const;

	// The status-line form: it names the fallback when there is one, so a project that has not been re-saved since
	// the tags arrived reads as "still loading these" rather than as an unexplained pause.
	FString DescribeStatus() const;
};

/**
 * The registry-only half of the Game Model container scan: it answers "which Blueprint asset is a container" from
 * the CrowdyScan / CrowdyContainerType asset-registry tags alone, with no package ever loaded. Split out of
 * FCrowdySchemaSync (CrowdyStudio, Private) into its own public surface so an editor-only tool outside CrowdyStudio
 * -- the schema retag console command -- can enumerate the same unmigrated assets a schema plan would otherwise
 * load, without pulling in the rest of the schema-diff machinery (which depends on private Studio types).
 */
class CROWDYSTUDIO_API FCrowdyContainerScan
{
public:
	// The content roots the scan sweeps: the scanned-roots setting when it names any, otherwise the game content
	// root plus the content of every plugin that could declare Crowdy metadata; the excluded-roots setting is then
	// subtracted from whichever applied. Trailing slashes are trimmed and duplicates collapsed.
	static TArray<FString> ResolveScannedRoots();

	// True when PackagePath is one of Roots or sits underneath one. Case-INSENSITIVE, because these come from a
	// hand-typed setting. An empty root never matches, since it would otherwise swallow the whole project.
	static bool IsPathUnderAnyRoot(const FString& PackagePath, const TArray<FString>& Roots);

	// The partition, as a pure function of what the registry reported: an asset whose scan version is current has
	// its container-hood answered from the tags, and everything else has to be loaded. Pure, so both halves of the
	// rule (the version compare and the container-tag presence) are testable without an asset.
	static FCrowdyContainerScanPlan PartitionScannedAssets(const TArray<FCrowdyScannedAsset>& Assets);

	// The registry query plus the partition above. Loads nothing at all, so a caller can build this to report what
	// a plan is about to do before it does it.
	static FCrowdyContainerScanPlan BuildContainerScanPlan();
};
