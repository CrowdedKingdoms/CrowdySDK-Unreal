#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

struct FGraphPanelNodeFactory;
class UClass;

// ─────────────────────────────────────────────────────────────────────────────
// Module-wide log category
// ─────────────────────────────────────────────────────────────────────────────
DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyEditor, Log, All)

// ─────────────────────────────────────────────────────────────────────────────
// Shared FName constants
// ─────────────────────────────────────────────────────────────────────────────
namespace CrowdyMetaKeys
{
	// Universal marker for any Crowdy function. On a "Crowdy Replicates" custom event it is
	// stamped onto the generated UFunction during Blueprint compilation (see the compiler
	// extension's ApplyReplicatedMeta) so TFieldIterator/the router find it.
	extern const FName CrowdyEvent;

	// Key-only UUserDefinedStruct metadata tags. These are inclusive flags
	// and can coexist with each other.
	extern const FName CrowdyPersistent;
	extern const FName CrowdySingleton;

	// Stamped onto the generated UClass during Blueprint compilation when the
	// class's component list contains a UCrowdyEntityComponent. Editor-only fast
	// path — at runtime UCrowdyAutoRegistry falls back to a construction-script /
	// CDO component walk, which works in packaged builds.
	extern const FName CrowdyEntity;
}

// ─────────────────────────────────────────────────────────────────────────────
// FCrowdySDKEditorModule
//
// Editor-only module providing:
//
//   • Right-click context menu entries on UUserDefinedStruct assets in the
//     Content Browser (Stamp as Event / ActorUpdate / Remove Stamp).
//     Implemented via UToolMenus extension — does NOT register a new asset
//     type and does NOT modify the struct editor's Details panel.
//
//   • Details panel customization for UK2Node_CustomEvent that adds the
//     "Crowdy Replicates" (RPC) checkbox + per-event routing dropdowns.
//
// ─────────────────────────────────────────────────────────────────────────────
class FCrowdySDKEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// Records a freshly recompiled class for an incremental CrowdyEvent rescan. The Blueprint
	// compiler extension calls this per class during a compile; OnBlueprintCompiled then refreshes
	// just these classes on the live registries at batch end, instead of resweeping every class.
	static void NotePendingRpcRescan(UClass* Class);

private:
	void RegisterStructContextMenu();
	void RegisterFunctionEntryCustomization();
	void RegisterCompilerExtension();
	void RegisterBlueprintCompilerExtension();
	void RegisterGraphNodeFactory();

	// After a compile reinstances a Blueprint class, refresh the live CrowdyEvent resolver so a
	// running PIE session picks up the recompiled functions and their new signature hashes.
	static void OnBlueprintCompiled();

	FDelegateHandle CompiledHandle;
	TSharedPtr<FGraphPanelNodeFactory> GraphNodeFactory;
};
