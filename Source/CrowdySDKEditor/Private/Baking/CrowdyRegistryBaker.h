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
	 */
	static void Rebuild(bool bDeep);

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
	 * cook/deep bake path). Also used by the Game Model schema sync so a container Blueprint that was marked
	 * but never opened this session is resident before class discovery iterates loaded classes. Heavy on a
	 * large project (holds the game thread while it loads), so callers run it behind a slow-task dialog.
	 */
	static void LoadAllTaggedAssets();

	/**
	 * The same set of assets, streamed in instead of force-loaded, with OnComplete invoked on the game thread once
	 * they are resident. This is what an interactive caller wants: the work is identical, but the editor stays usable
	 * while it happens, where the synchronous form above holds the game thread for as long as the loads take (measured
	 * at seconds on a real project, behind a modal dialog that could not even be cancelled).
	 *
	 * Falls back to the synchronous load in a commandlet/cook context, where there is no editor to keep responsive.
	 * A re-request while one is already in flight is refused: OnComplete is still invoked, because a caller that
	 * cannot rely on it firing has to invent its own timeout.
	 */
	static void LoadAllTaggedAssetsAsync(TFunction<void()> OnComplete);

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
	// then invoke OnComplete. Runs on the game thread.
	static void FinishRebuild(TFunction<void()> OnComplete);

	static void SaveAsset(UCrowdyBakedRegistry* Registry);

	static void OnStartup();

	/** Fired at the start of every cook: deep-bakes and injects the asset. */
	static void OnModifyCook(TConstArrayView<const ITargetPlatform*> TargetPlatforms,
		TArray<FName>& PackagesToCook, TArray<FName>& PackagesToNeverCook);
};
