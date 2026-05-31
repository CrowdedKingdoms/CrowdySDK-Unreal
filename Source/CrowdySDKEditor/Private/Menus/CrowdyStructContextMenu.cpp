#include "Menus/CrowdyStructContextMenu.h"
#include "CrowdySDKEditor.h"
#include "ToolMenus.h"
#include "ToolMenu.h"
#include "ToolMenuSection.h"
#include "ToolMenuEntry.h"
#include "ContentBrowserMenuContexts.h"
#include "Engine/UserDefinedStruct.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "StructUtils/UserDefinedStruct.h"

// ─────────────────────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────────────────────
const FName FCrowdyStructContextMenu::OwnerName(TEXT("CrowdySDKEditor.StructContextMenu"));

namespace CrowdyRepValues
{
	static const FString Event       (TEXT("Event"));
	static const FString ActorUpdate (TEXT("ActorUpdate"));
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
		FName(TEXT("CrowdyRepType")),
		FText::FromString(TEXT("Crowdy Replication Type")),
		FText::FromString(TEXT("Exclusive CrowdyRep payload type for this struct.")),
		FNewToolMenuChoice(FNewMenuDelegate::CreateStatic(
			&FCrowdyStructContextMenu::PopulateRepSubMenu,
			Structs)));

	InSection.AddSubMenu(
		FName(TEXT("CrowdyMetadataFlags")),
		FText::FromString(TEXT("Crowdy Metadata Flags")),
		FText::FromString(TEXT("Inclusive key-only Crowdy metadata flags for this struct.")),
		FNewToolMenuChoice(FNewMenuDelegate::CreateStatic(
			&FCrowdyStructContextMenu::PopulateFlagsSubMenu,
			Structs)));
}

void FCrowdyStructContextMenu::PopulateRepSubMenu(
	FMenuBuilder& MenuBuilder,
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs)
{
	// ── Stamp as Event ───────────────────────────────────────────────────────
	MenuBuilder.AddMenuEntry(
		FText::FromString(TEXT("None")),
		FText::FromString(
			TEXT("Removes the CrowdyRep metadata from this struct.\n"
			     "Inclusive Crowdy metadata flags are left unchanged.")),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(
				&FCrowdyStructContextMenu::ExecuteRemoveStamp,
				Structs),
			FCanExecuteAction::CreateStatic(
				&FCrowdyStructContextMenu::CanRemoveStamp,
				Structs),
			FIsActionChecked::CreateStatic(
				&FCrowdyStructContextMenu::IsRepSet,
				Structs,
				FString())),
		FName(TEXT("CrowdyRemoveRep")),
		EUserInterfaceActionType::RadioButton);

	MenuBuilder.AddMenuEntry(
		FText::FromString(TEXT("Crowdy Event")),
		FText::FromString(
			TEXT("Tags this struct with meta=(CrowdyRep=\"Event\").\n"
			     "UCrowdyEventManager will dispatch instances of this struct\n"
			     "to registered handlers at runtime.")),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(
				&FCrowdyStructContextMenu::ExecuteStampAs,
				Structs,
				CrowdyRepValues::Event),
			FCanExecuteAction::CreateStatic(
				&FCrowdyStructContextMenu::CanStampAs,
				Structs,
				CrowdyRepValues::Event),
			FIsActionChecked::CreateStatic(
				&FCrowdyStructContextMenu::IsRepSet,
				Structs,
				CrowdyRepValues::Event)),
		FName(TEXT("CrowdyStampAsEvent")),
		EUserInterfaceActionType::RadioButton);

	// ── Stamp as ActorUpdate ─────────────────────────────────────────────────
	MenuBuilder.AddMenuEntry(
		FText::FromString(TEXT("Crowdy Actor Update")),
		FText::FromString(
			TEXT("Tags this struct with meta=(CrowdyRep=\"ActorUpdate\").\n"
			     "UCrowdyActorTracker will process instances of this struct\n"
			     "to update tracked actor state at runtime.")),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(
				&FCrowdyStructContextMenu::ExecuteStampAs,
				Structs,
				CrowdyRepValues::ActorUpdate),
			FCanExecuteAction::CreateStatic(
				&FCrowdyStructContextMenu::CanStampAs,
				Structs,
				CrowdyRepValues::ActorUpdate),
			FIsActionChecked::CreateStatic(
				&FCrowdyStructContextMenu::IsRepSet,
				Structs,
				CrowdyRepValues::ActorUpdate)),
		FName(TEXT("CrowdyStampAsActorUpdate")),
		EUserInterfaceActionType::RadioButton);
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
		FText::FromString(TEXT("Crowdy Instanced")),
		FText::FromString(TEXT("Toggles meta=(CrowdyInstanced) on this struct.")),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateStatic(
				&FCrowdyStructContextMenu::ExecuteSetFlag,
				Structs,
				CrowdyMetaKeys::CrowdyInstanced,
				!IsFlagSet(Structs, CrowdyMetaKeys::CrowdyInstanced)),
			FCanExecuteAction(),
			FIsActionChecked::CreateStatic(
				&FCrowdyStructContextMenu::IsFlagSet,
				Structs,
				CrowdyMetaKeys::CrowdyInstanced)),
		FName(TEXT("CrowdyInstanced")),
		EUserInterfaceActionType::ToggleButton);
}

// ─────────────────────────────────────────────────────────────────────────────
// Predicates
//
// NOTE: by VALUE (not const-ref) because FCanExecuteAction::CreateStatic uses
// std::decay_t on bound args; the function pointer signature must match the
// decayed type exactly.
// ─────────────────────────────────────────────────────────────────────────────
bool FCrowdyStructContextMenu::CanStampAs(
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
	FString TargetCategory)
{
	// Enable if any selected struct does NOT already have this exact category.
	for (const TWeakObjectPtr<UUserDefinedStruct>& WeakStruct : Structs)
	{
		if (!WeakStruct.IsValid()) continue;

		const FString Existing =
			WeakStruct->GetMetaData(CrowdyMetaKeys::CrowdyRep);

		if (Existing != TargetCategory)
			return true;
	}
	return false;
}

bool FCrowdyStructContextMenu::CanRemoveStamp(
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs)
{
	for (const TWeakObjectPtr<UUserDefinedStruct>& WeakStruct : Structs)
	{
		if (!WeakStruct.IsValid()) continue;

		if (!WeakStruct->GetMetaData(CrowdyMetaKeys::CrowdyRep).IsEmpty()
			|| !WeakStruct->GetMetaData(CrowdyMetaKeys::LegacyCrowdyCategory).IsEmpty())
			return true;
	}
	return false;
}

bool FCrowdyStructContextMenu::IsRepSet(
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
	FString TargetCategory)
{
	for (const TWeakObjectPtr<UUserDefinedStruct>& WeakStruct : Structs)
	{
		if (!WeakStruct.IsValid()) continue;

		if (WeakStruct->GetMetaData(CrowdyMetaKeys::CrowdyRep) == TargetCategory)
		{
			return true;
		}
	}
	return false;
}

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
void FCrowdyStructContextMenu::ExecuteStampAs(
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
	FString Category)
{
	for (TWeakObjectPtr<UUserDefinedStruct>& WeakStruct : Structs)
	{
		if (!WeakStruct.IsValid()) continue;

		UUserDefinedStruct* Struct = WeakStruct.Get();
		Struct->SetMetaData(CrowdyMetaKeys::CrowdyRep, *Category);
		Struct->RemoveMetaData(CrowdyMetaKeys::LegacyCrowdyCategory);
		Struct->MarkPackageDirty();

		UE_LOG(LogCrowdyEditor, Log,
			TEXT("[CrowdySDK] Stamped struct '%s' CrowdyRep=%s. Save to persist."),
			*Struct->GetName(), *Category);
	}
}

void FCrowdyStructContextMenu::ExecuteRemoveStamp(
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs)
{
	for (TWeakObjectPtr<UUserDefinedStruct>& WeakStruct : Structs)
	{
		if (!WeakStruct.IsValid()) continue;

		UUserDefinedStruct* Struct = WeakStruct.Get();
		Struct->RemoveMetaData(CrowdyMetaKeys::CrowdyRep);
		Struct->RemoveMetaData(CrowdyMetaKeys::LegacyCrowdyCategory);
		Struct->MarkPackageDirty();

		UE_LOG(LogCrowdyEditor, Log,
			TEXT("[CrowdySDK] Removed Crowdy stamp from struct '%s'. Save to persist."),
			*Struct->GetName());
	}
}

void FCrowdyStructContextMenu::ExecuteSetFlag(
	TArray<TWeakObjectPtr<UUserDefinedStruct>> Structs,
	FName MetaKey,
	bool bEnabled)
{
	for (TWeakObjectPtr<UUserDefinedStruct>& WeakStruct : Structs)
	{
		if (!WeakStruct.IsValid()) continue;

		UUserDefinedStruct* Struct = WeakStruct.Get();
		if (bEnabled)
		{
			Struct->SetMetaData(MetaKey, TEXT(""));
		}
		else
		{
			Struct->RemoveMetaData(MetaKey);
		}

		Struct->MarkPackageDirty();

		UE_LOG(LogCrowdyEditor, Log,
			TEXT("[CrowdySDK] %s key-only metadata '%s' on struct '%s'. Save to persist."),
			bEnabled ? TEXT("Set") : TEXT("Removed"),
			*MetaKey.ToString(),
			*Struct->GetName());
	}
}
