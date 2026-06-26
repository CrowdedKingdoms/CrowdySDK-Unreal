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
	// The signed-in editor token (the email/password session stored in the token vault), or empty
	// when not signed in. Lets other editor modules issue authenticated edit-time queries — e.g. the
	// Blueprint channel picker listing an app's channels. Exported so CrowdySDKEditor can read it.
	CROWDYSTUDIO_API FString GetSignedInToken();
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
	// an OnComplete callback it invokes on the game thread once the bake finishes — that is when the
	// Registry page refreshes its view. OnComplete is also called on the no-hook path so callers can
	// rely on it always firing.
	CROWDYSTUDIO_API void SetRebuildHook(TFunction<void(TFunction<void()> /*OnComplete*/)> Hook);
	CROWDYSTUDIO_API bool HasRebuildHook();
	CROWDYSTUDIO_API void RequestRebuild(TFunction<void()> OnComplete = nullptr);
}

/**
 * Editor-only module for the Crowded Kingdoms management console. It owns a single
 * dockable "CrowdyStudio" tab and a Tools-menu entry that opens it. The console UI
 * itself arrives in Phase 1; Phase 0 only stands up the tab and the shared plumbing.
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
