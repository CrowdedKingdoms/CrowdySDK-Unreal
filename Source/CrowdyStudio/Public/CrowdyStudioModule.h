// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Templates/Function.h"

class SDockTab;
class SWidget;
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
}

namespace CrowdyStudioExtraPages
{
	// Adds a page to the console's navigation, after its own pages under a group of its own, for an editor module
	// the console cannot depend on. Icon is a console icon name ("server", "apps", ...). The console reads the pages
	// when its window opens and calls MakeWidget once per window; registering an Id again replaces its page. Pages
	// need a sign-in, like the console's authoring pages.
	CROWDYSTUDIO_API void RegisterPage(FName Id, const FText& Group, const FText& Label, const TCHAR* Icon,
		TFunction<TSharedRef<SWidget>()> MakeWidget);
	CROWDYSTUDIO_API void UnregisterPage(FName Id);

	// Opens the console's tab on that page; while nobody is signed in it opens on Sign In and shows the page after.
	CROWDYSTUDIO_API void OpenPage(FName Id);
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
