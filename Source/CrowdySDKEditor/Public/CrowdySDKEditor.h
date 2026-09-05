#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

struct FGraphPanelNodeFactory;
struct FGraphPanelPinFactory;
class IAssetTypeActions;
class UClass;

// Module-wide log category.
DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyEditor, Log, All)

// FCrowdySDKEditorModule is the editor-only module providing:
//   - Right-click context menu entries on UUserDefinedStruct assets in the Content Browser
//     (Stamp as Event / ActorUpdate / Remove Stamp). Implemented via a UToolMenus extension; does NOT
//     register a new asset type and does NOT modify the struct editor's Details panel.
//   - Details panel customization for UK2Node_CustomEvent that adds the "Crowdy Replicates" (RPC)
//     checkbox + per-event routing dropdowns.
//   - Details panel customization for a Blueprint variable that adds the "Crowdy Replication" mode
//     dropdown (the Replicated mode writes the CrowdyState metadata).
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

	// Registers FCrowdyEffectCustomization with the Blueprint-agnostic PropertyEditor module so a UCrowdyEffect
	// asset's Details panel gets the constrained picker over its structured spec. Unregistered in
	// ShutdownModule alongside the K2Node_CustomEvent layout.
	void RegisterEffectCustomization();

	// Registers FCrowdyReplicatedVariableCustomization with the Blueprint editor ("Kismet") so a Blueprint
	// variable's Details panel gets the "Crowdy Replication" mode dropdown (the Replicated mode stamps the
	// CrowdyState metadata). Unregistered in ShutdownModule via BlueprintVariableCustomizationHandle.
	void RegisterVariableCustomization();

	void RegisterCompilerExtension();

	// Supplies the Blueprint compile pass (which lives in CrowdyNodes, so it also runs in an uncooked
	// non-editor process) with the parts of its work only an editor can do: stamping the Game Model
	// container tags from the persisted marker, and keeping the cooked registry bake and a live PIE
	// registry in step. Cleared in ShutdownModule so the lambdas never dangle.
	void InstallBlueprintCompileHooks();

	void RegisterGraphNodeFactory();

	// Registers FCrowdyEffectAssetTypeActions so a Graph-sourced UCrowdyEffect opens in the node-graph asset editor;
	// non-graph effects still open in the default property editor. Unregistered in ShutdownModule.
	void RegisterEffectAssetTypeActions();

	// Registers FCrowdyStatePropertyPinFactory so the Property Name pin of Mark Crowdy State Dirty renders a
	// dropdown of the target actor class's CrowdyManualDirty properties. Unregistered in ShutdownModule.
	void RegisterGraphPinFactory();

	// After a compile reinstances a Blueprint class, refresh the live CrowdyEvent resolver so a
	// running PIE session picks up the recompiled functions and their new signature hashes.
	static void OnBlueprintCompiled();

	FDelegateHandle CompiledHandle;

	// Handle for the registered Blueprint-variable Details customization (from the Kismet module's
	// RegisterVariableCustomization), unregistered in ShutdownModule.
	FDelegateHandle BlueprintVariableCustomizationHandle;

	TSharedPtr<FGraphPanelNodeFactory> GraphNodeFactory;

	// Visual pin factory for Mark Crowdy State Dirty's Property Name dropdown; unregistered in ShutdownModule.
	TSharedPtr<FGraphPanelPinFactory> GraphPinFactory;

	// Registered asset type actions for UCrowdyEffect (opens the graph editor for Graph-sourced effects);
	// unregistered in ShutdownModule.
	TSharedPtr<IAssetTypeActions> EffectAssetTypeActions;
};
