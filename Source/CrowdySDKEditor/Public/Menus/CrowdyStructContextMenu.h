#pragma once

#include "CoreMinimal.h"
struct FToolMenuSection;
class FMenuBuilder;
class UToolMenu;
class UUserDefinedStruct;

// ─────────────────────────────────────────────────────────────────────────────
// FCrowdyStructContextMenu
//
// Adds three Crowdy entries to the right-click menu of UserDefinedStruct
// assets in the Content Browser. Does NOT register a new asset type and does
// NOT touch the struct editor's Details panel.
//
// The previous design used FAssetTypeActions_Base, which has the unfortunate
// side effect of also registering the type as a creatable asset under
// Add → Blueprint → <Name>. UToolMenus::ExtendMenu is the correct mechanism
// for "add a menu entry to existing assets" without claiming asset-type
// ownership.
//
// Menu identifier: "ContentBrowser.AssetContextMenu.UserDefinedStruct"
//
// This identifier is the engine-generated context menu specifically for
// UUserDefinedStruct assets. Our entries only appear when the user right-
// clicks a struct asset — never on other asset types.
// ─────────────────────────────────────────────────────────────────────────────
class FCrowdyStructContextMenu
{
public:
	// Called from FCrowdySDKEditorModule::StartupModule, deferred via
	// UToolMenus::RegisterStartupCallback so we extend the menu only once
	// the ToolMenus system is initialised.
	static void Register();

	// Called from ShutdownModule. Removes our owner registration so the
	// entries disappear cleanly on plugin unload / editor shutdown.
	static void Unregister();

private:
	// Populates the menu section when the context menu is opened.
	// Uses UContentBrowserAssetContextMenuContext to access the selected assets.
	static void PopulateMenuSection(struct FToolMenuSection& InSection);
	static void PopulateRepSubMenu(
		FMenuBuilder& MenuBuilder,
		TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs);

	static void PopulateFlagsSubMenu(
		FMenuBuilder& MenuBuilder,
		TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs);

	// Executors — operate on the selected struct asset(s).
	static void ExecuteStampAs(
		TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
		FString Category);

	static void ExecuteRemoveStamp(
		TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs);

	static void ExecuteSetFlag(
		TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
		FName MetaKey,
		bool bEnabled);

	// Predicates — controls whether the menu entries are enabled.
	static bool CanStampAs(
		TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
		FString TargetCategory);

	static bool CanRemoveStamp(
		TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs);

	static bool IsRepSet(
		TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
		FString TargetCategory);

	static bool IsFlagSet(
		TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
		FName MetaKey);

	// Owner used for UToolMenus::UnregisterOwner. A static FName instance
	// keeps the registration tied to this class.
	static const FName OwnerName;
};
