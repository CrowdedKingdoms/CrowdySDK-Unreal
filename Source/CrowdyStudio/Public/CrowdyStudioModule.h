// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Templates/Function.h"

class SDockTab;
class FSpawnTabArgs;

DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyStudio, Log, All);

namespace CrowdyStudioAuth
{
	// The signed-in editor token (the session stored in the token vault), or empty when not signed in.
	// Lets other editor modules tell whether a Crowdy Studio sign-in exists at all. NOTE: this is the
	// identity SESSION token the Game API rejects it, so it must NOT be sent to a game-plane endpoint;
	// mint an app-scoped token first (see FetchAppChannelNames). Exported so CrowdySDKEditor can read it.
	CROWDYSTUDIO_API FString GetSignedInToken();

	// Fetches the app's channel names for the Blueprint channel picker. Channels live on the Game API,
	// which only accepts an app-scoped token, so this mints one from the signed-in session token (like
	// the console does) and then queries channels the session token alone is rejected. Async; OnDone
	// runs on the game thread with the names (empty on any failure: not signed in, no session scope,
	// mint rejected, or the query failed). Exported so CrowdySDKEditor's picker can call it.
	CROWDYSTUDIO_API void FetchAppChannelNames(int64 AppId, TFunction<void(const TArray<FString>&)> OnDone);
}

namespace CrowdyStudioTrace
{
	// crowdy.studio.trace: when set, logs each console GraphQL op including its plane (management or
	// game), the operation name, and the request outcome (HTTP code, error count). The bearer token
	// is never logged. Off by default, in the spirit of crowdy.rpc.trace.
	CROWDYSTUDIO_API bool Enabled();
}

namespace CrowdyStudioRegistry
{
	// The deep-rebuild of the baked metadata registry lives in the editor-only baker
	// (UCrowdyRegistryBaker, in CrowdySDKEditor), which already depends on CrowdyStudio. This hook
	// lets that module supply the rebuild action to the console's Registry page without CrowdyStudio
	// depending back on it (which would be a module cycle). When no hook is set the Registry page's
	// Rebuild button is disabled; Refresh (re-reading the asset) still works.
	//
	// The rebuild is asynchronous (it streams assets without freezing the editor), so the hook takes
	// an OnComplete callback it invokes on the game thread once the bake finishes that is when the
	// Registry page refreshes its view. OnComplete is also called on the no-hook path so callers can
	// rely on it always firing.
	CROWDYSTUDIO_API void SetRebuildHook(TFunction<void(TFunction<void()> /*OnComplete*/)> Hook);
	CROWDYSTUDIO_API bool HasRebuildHook();
	CROWDYSTUDIO_API void RequestRebuild(TFunction<void()> OnComplete = nullptr);

	// A Blueprint marked as a Game Model container (its persisted CrowdyContainerBlueprintExtension) that has
	// not been opened this editor session is not loaded, so the schema sync's class iteration would miss it.
	// The load lives in the editor-only baker (UCrowdyRegistryBaker, in CrowdySDKEditor); this hook lets that module
	// supply it without CrowdyStudio depending back on it (which would be a module cycle), mirroring SetRebuildHook
	// above.
	//
	// ASYNCHRONOUS, and OnComplete is what carries the plan forward: the assets are streamed rather than force-loaded,
	// because the synchronous form held the game thread for seconds on a real project behind a modal dialog. Like
	// RequestRebuild, OnComplete always fires, including when no hook is set (the sync then proceeds with only the
	// containers already loaded, which is a smaller answer rather than a wrong one) and when the request is refused.
	CROWDYSTUDIO_API void SetLoadContainerAssetsHook(TFunction<void(TFunction<void()> /*OnComplete*/)> Hook);
	CROWDYSTUDIO_API void RequestLoadContainerAssets(TFunction<void()> OnComplete);
	// A copy of the installed hook, so a caller that swaps it can put the real one back. The always-completes contract
	// above is only testable by removing the hook, and a test that left the editor module's hook uninstalled would
	// quietly disable unopened-container discovery for everything that ran after it.
	CROWDYSTUDIO_API TFunction<void(TFunction<void()>)> GetLoadContainerAssetsHook();
}

/**
 * Editor-only module for the Crowded Kingdoms management console. It owns a single
 * dockable "CrowdyStudio" tab and a Tools-menu entry that opens it. This module only
 * stands up the tab and the shared plumbing; the console UI itself lives elsewhere.
 */
class FCrowdyStudioModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
#if WITH_EDITOR
	void RegisterTabSpawner();
	void RegisterMenus();
	// Adds both the Tools-menu entry and the main-toolbar button (next to Play).
	void ExtendEditorMenus();
	void ExtendToolsMenu();
	// A "Crowdy Studio" button in the level-editor play toolbar, right of the Play controls (mirrors
	// where the mod.io button sits), so the console is reachable without hunting through Tools.
	void ExtendLevelEditorToolbar();
	void OpenStudioTab();
	TSharedRef<SDockTab> SpawnStudioTab(const FSpawnTabArgs& Args);
#endif
};
