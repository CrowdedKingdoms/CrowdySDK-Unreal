// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "UObject/Object.h"
#include "CrowdyRegistryBaker.generated.h"

class UCrowdyBakedRegistry;
class ITargetPlatform;
struct FAssetData;

/**
 * Editor-only baker. Reads the Crowdy meta=(...) keys (which exist only while
 * WITH_METADATA is set, i.e. in editor builds) and writes them into the
 * UCrowdyBakedRegistry data asset that cooks into the packaged game. The runtime
 * reads that asset instead of the stripped metadata.
 *
 * Triggers (registered from FCrowdySDKEditorModule::StartupModule):
 *   - Tools > Rebuild Crowdy Registry: full deep rebuild + save (run before packaging)
 *   - Editor startup: first-run bake if the asset is missing
 *   - Blueprint compile: incremental refresh of the compiled class
 */
UCLASS()
class UCrowdyRegistryBaker : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Full rebuild of the baked asset from currently-known metadata.
	 * @param bDeep  Load every Blueprint and User Defined Struct via the asset
	 *               registry first, so assets never opened this session are
	 *               captured. Heavier; used by the manual menu action.
	 * @param bNotify  Shows an editor toast with the outcome. Only the interactive menu action sets it.
	 * @return false when the baked asset could not be created or saved.
	 */
	static bool Rebuild(bool bDeep, bool bNotify = false);

	/**
	 * Interactive deep rebuild for the CrowdyStudio Registry page button. Rebuild(true) force-loads
	 * every tagged asset synchronously, which freezes the editor for as long as that takes on a large
	 * project. This instead streams them in asynchronously so the editor stays responsive, then bakes,
	 * saves and invokes OnComplete on the game thread. Falls back to the synchronous Rebuild in a
	 * commandlet/cook context; a re-request while one is already in flight is ignored.
	 */
	static void RebuildAsync(TFunction<void()> OnComplete = nullptr);

	/** Incrementally refreshes a single class's entries (called on BP compile). */
	static void UpdateForClass(UClass* Class);

	/**
	 * Registers every trigger: the cook-time rebuild (the one that makes this
	 * fully automatic), the Tools-menu entry, the editor-startup first-run bake,
	 * and the per-Blueprint-compile incremental refresh.
	 */
	static void Register();

	/**
	 * Synchronously force-loads every tagged Blueprint / User Defined Struct in project + SDK content (the
	 * cook/deep bake path). Heavy on a large project: it holds the game thread while it loads.
	 */
	static void LoadAllTaggedAssets();

	/**
	 * Sweeps every loaded class and struct into Registry, replacing whatever it held. This is the whole bake; the
	 * rebuild paths above only decide what is resident before it runs and what happens to the asset afterwards.
	 * Public so a test can bake into a throwaway registry and read what came out.
	 */
	static void PopulateFromLoadedObjects(UCrowdyBakedRegistry* Registry);

private:
	/** Loads the configured asset, or null if none is assigned/loadable. */
	static UCrowdyBakedRegistry* ResolveAsset();

	/** Loads the configured asset, creating + assigning one if none exists. */
	static UCrowdyBakedRegistry* ResolveOrCreateAsset();

	// Asset-registry query for every tagged Blueprint / User Defined Struct, without loading them.
	static void GatherTaggedAssets(TArray<FAssetData>& OutAssets);

	// Shared tail of a rebuild: sweep loaded objects into the asset, save, invalidate the cache,
	// then invoke OnComplete. Runs on the game thread. Returns false when the asset could not be created or saved.
	static bool FinishRebuild(TFunction<void()> OnComplete, bool bNotify = false);

	static bool SaveAsset(UCrowdyBakedRegistry* Registry);

	static void OnStartup();

	/** Fired at the start of every cook: deep-bakes and injects the asset. */
	static void OnModifyCook(TConstArrayView<const ITargetPlatform*> TargetPlatforms,
		TArray<FName>& PackagesToCook, TArray<FName>& PackagesToNeverCook);
};
