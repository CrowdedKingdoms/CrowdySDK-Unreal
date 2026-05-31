#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

struct FGraphPanelNodeFactory;

// ─────────────────────────────────────────────────────────────────────────────
// Module-wide log category
// ─────────────────────────────────────────────────────────────────────────────
DECLARE_LOG_CATEGORY_EXTERN(LogCrowdyEditor, Log, All)

// ─────────────────────────────────────────────────────────────────────────────
// Shared FName constants
// ─────────────────────────────────────────────────────────────────────────────
namespace CrowdyMetaKeys
{
	// On Blueprint function/custom event node metadata; transferred to the
	// generated UFunction at pre-compile time so TFieldIterator finds it.
	extern const FName CrowdyEvent;

	// On UUserDefinedStruct metadata to tag payload replication category.
	extern const FName CrowdyRep;
	extern const FName LegacyCrowdyCategory;

	// Key-only UUserDefinedStruct metadata tags. These are inclusive flags
	// and can coexist with each other and with CrowdyRep.
	extern const FName CrowdyPersistent;
	extern const FName CrowdyInstanced;
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
//   • Details panel customization for UK2Node_FunctionEntry that adds a
//     "Crowdy Event Handler" checkbox + signature validation row. The same
//     customization is also available on UK2Node_CustomEvent.
//
//   • Pre-compile hook that transfers CrowdyEvent metadata from Blueprint
//     function/custom event nodes onto the generated UFunction.
//
// ─────────────────────────────────────────────────────────────────────────────
class FCrowdySDKEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterStructContextMenu();
	void RegisterFunctionEntryCustomization();
	void RegisterCompilerExtension();
	void RegisterBlueprintCompilerExtension();
	void RegisterGraphNodeFactory();

	static void OnBlueprintPreCompile(UBlueprint* Blueprint);

	FDelegateHandle PreCompileHandle;
	TSharedPtr<FGraphPanelNodeFactory> GraphNodeFactory;
};
