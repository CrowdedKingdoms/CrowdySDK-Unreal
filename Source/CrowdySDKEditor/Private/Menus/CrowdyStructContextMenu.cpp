#include "Menus/CrowdyStructContextMenu.h"
#include "CrowdySDKEditor.h"
#include "Menus/CrowdyStructMetaUtils.h"
#include "ToolMenus.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenuEntry.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserMenuContexts.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/AssetRegistryTagsContext.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
const FName FCrowdyStructContextMenu::OwnerName(TEXT("CrowdySDKEditor.StructContextMenu"));

namespace CrowdyRepValues
{
	static const TCHAR* EnabledFlagValue = TEXT("true");
}

// Menu identifier for the UserDefinedStruct-specific context menu.
// Verified via the `ToolMenus.Edit` console command in 5.x.
static const FName StructAssetContextMenuName(
	TEXT("ContentBrowser.AssetContextMenu.UserDefinedStruct"));

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────
void FCrowdyStructContextMenu::Register()
{
	// Defer until ToolMenus is ready. RegisterStartupCallback handles the case
	// where ToolMenus initialises before/after our module loads.
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
		{
			// FToolMenuOwnerScoped tags every entry added inside its lifetime
			// with our owner name. UnregisterOwnerByName(OwnerName) later
			// removes only those entries, leaving engine entries untouched.
			FToolMenuOwnerScoped OwnerScope(OwnerName);

			UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(StructAssetContextMenuName);
			if (!Menu) return;

			// Dedicated "Crowdy SDK" section so our entries group together
			// and are visually separated from engine entries.
			FToolMenuSection& Section = Menu->FindOrAddSection(
				FName(TEXT("CrowdySDK")),
				FText::FromString(TEXT("Crowdy SDK")));

			// Dynamic entry: the callback runs every time the menu is opened,
			// so we always see fresh asset state and current stamp metadata.
			Section.AddDynamicEntry(
				FName(TEXT("CrowdyStructStamp")),
				FNewToolMenuSectionDelegate::CreateStatic(
					&FCrowdyStructContextMenu::PopulateMenuSection));
		}));
}

void FCrowdyStructContextMenu::Unregister()
{
	// Guard against shutdown ordering — UToolMenus may already be torn down.
	if (UObjectInitialized() && UToolMenus::Get())
		UToolMenus::Get()->UnregisterOwnerByName(OwnerName);
}

// ─────────────────────────────────────────────────────────────────────────────
// Dynamic menu population
//
// Runs every time the user opens the context menu, so we see fresh state.
// UContentBrowserAssetContextMenuContext provides SelectedAssets — we filter
// to UUserDefinedStruct pointers and bind them into the actions.
// ─────────────────────────────────────────────────────────────────────────────
void FCrowdyStructContextMenu::PopulateMenuSection(FToolMenuSection& InSection)
{
	const UContentBrowserAssetContextMenuContext* Context =
		InSection.FindContext<UContentBrowserAssetContextMenuContext>();
	if (!Context) return;

	// Collect the selected structs as weak pointers so a delayed click after
	// the asset closes/reloads doesn't crash.
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs;
	Structs.Reserve(Context->SelectedAssets.Num());

	for (const FAssetData& AssetData : Context->SelectedAssets)
	{
		if (UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(AssetData.GetAsset()))
			Structs.Add(Struct);
	}

	if (Structs.IsEmpty()) return;

	InSection.AddSubMenu(
		FName(TEXT("CrowdyMetadataFlags")),
		FText::FromString(TEXT("Crowdy Metadata Flags")),
		FText::FromString(TEXT("Inclusive key-only Crowdy metadata flags for this struct.")),
		FNewToolMenuChoice(FNewMenuDelegate::CreateStatic(
			&FCrowdyStructContextMenu::PopulateFlagsSubMenu,
			Structs)));
}

void FCrowdyStructContextMenu::PopulateFlagsSubMenu(
	FMenuBuilder& MenuBuilder,
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs)
{
	MenuBuilder.AddMenuEntry(
		FText::FromString(TEXT("Crowdy Persistent")),
		FText::FromString(TEXT("Toggles meta=(CrowdyPersistent) on this struct.")),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(
				&FCrowdyStructContextMenu::ExecuteSetFlag,
				Structs,
				CrowdyMetaKeys::CrowdyPersistent,
				!IsFlagSet(Structs, CrowdyMetaKeys::CrowdyPersistent)),
			FCanExecuteAction(),
			FIsActionChecked::CreateStatic(
				&FCrowdyStructContextMenu::IsFlagSet,
				Structs,
				CrowdyMetaKeys::CrowdyPersistent)),
		FName(TEXT("CrowdyPersistent")),
		EUserInterfaceActionType::ToggleButton);

	MenuBuilder.AddMenuEntry(
		FText::FromString(TEXT("Crowdy Singleton")),
		FText::FromString(TEXT("Toggles meta=(CrowdySingleton) on this struct. By default all persistent structs are instanced.")),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(
				&FCrowdyStructContextMenu::ExecuteSetFlag,
				Structs,
				CrowdyMetaKeys::CrowdySingleton,
				!IsFlagSet(Structs, CrowdyMetaKeys::CrowdySingleton)),
			FCanExecuteAction(),
			FIsActionChecked::CreateStatic(
				&FCrowdyStructContextMenu::IsFlagSet,
				Structs,
				CrowdyMetaKeys::CrowdySingleton)),
		FName(TEXT("CrowdySingleton")),
		EUserInterfaceActionType::ToggleButton);
}

// ─────────────────────────────────────────────────────────────────────────────
// Predicates
//
// NOTE: by VALUE (not const-ref) because FCanExecuteAction::CreateStatic uses
// std::decay_t on bound args; the function pointer signature must match the
// decayed type exactly.
// ─────────────────────────────────────────────────────────────────────────────
bool FCrowdyStructContextMenu::IsFlagSet(
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
	FName MetaKey)
{
	bool bSawValidStruct = false;
	for (const TWeakObjectPtr<UUserDefinedStruct>& WeakStruct : Structs)
	{
		if (!WeakStruct.IsValid()) continue;

		bSawValidStruct = true;
		if (!WeakStruct->HasMetaData(MetaKey))
		{
			return false;
		}
	}
	return bSawValidStruct;
}

// ─────────────────────────────────────────────────────────────────────────────
// Executors
// ─────────────────────────────────────────────────────────────────────────────
void FCrowdyStructContextMenu::ExecuteSetFlag(
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
	FName MetaKey,
	bool bEnabled)
{
	for (TWeakObjectPtr<UUserDefinedStruct>& WeakStruct : Structs)
	{
		if (!WeakStruct.IsValid()) continue;

		UUserDefinedStruct* Struct = WeakStruct.Get();
		Struct->Modify();
		if (bEnabled)
		{
			Struct->SetMetaData(MetaKey, CrowdyRepValues::EnabledFlagValue);
		}
		else
		{
			Struct->RemoveMetaData(MetaKey);
		}

		NotifyStructMetadataChanged(Struct);

		UE_LOG(LogCrowdyEditor, Log,
			TEXT("[CrowdySDK] %s key-only metadata '%s' on struct '%s'. Save to persist."),
			bEnabled ? TEXT("Set") : TEXT("Removed"),
			*MetaKey.ToString(),
			*Struct->GetName());
	}
}
