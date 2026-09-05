#include "Menus/CrowdyContainerEditorToolbar.h"

#include "Engine/Blueprint.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameModel/CrowdyContainerBlueprintExtension.h"
#include "Menus/CrowdyToolbarStyle.h"
#include "Styling/SlateTypes.h" // ECheckBoxState
#include "Textures/SlateIcon.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Widgets/Input/SEditableTextBox.h"

#define LOCTEXT_NAMESPACE "CrowdyContainerEditorToolbar"

FDelegateHandle FCrowdyContainerEditorToolbar::ToolbarExtenderHandle;

namespace
{
	// A Game Model container is a normal class (actor or plain UObject). Interfaces, macro libraries, and level
	// scripts can never be containers, so the Crowdy toolbar menu is not offered for them.
	bool IsContainerCandidate(const UBlueprint* Blueprint)
	{
		return Blueprint && Blueprint->BlueprintType == BPTYPE_Normal;
	}
}

void FCrowdyContainerEditorToolbar::Register()
{
	TArray<FAssetEditorExtender>& ToolbarExtenders =
		FAssetEditorToolkit::GetSharedToolBarExtensibilityManager()->GetExtenderDelegates();

	const int32 NewIndex = ToolbarExtenders.Add(
		FAssetEditorExtender::CreateStatic(&FCrowdyContainerEditorToolbar::CreateToolbarExtender));

	ToolbarExtenderHandle = ToolbarExtenders[NewIndex].GetHandle();
}

void FCrowdyContainerEditorToolbar::Unregister()
{
	if (!ToolbarExtenderHandle.IsValid())
	{
		return;
	}

	TArray<FAssetEditorExtender>& ToolbarExtenders =
		FAssetEditorToolkit::GetSharedToolBarExtensibilityManager()->GetExtenderDelegates();

	ToolbarExtenders.RemoveAll([](const FAssetEditorExtender& Extender)
	{
		return Extender.GetHandle() == ToolbarExtenderHandle;
	});

	ToolbarExtenderHandle.Reset();
}

TSharedRef<FExtender> FCrowdyContainerEditorToolbar::CreateToolbarExtender(
	const TSharedRef<FUICommandList> CommandList,
	const TArray<UObject*> EditingObjects)
{
	TSharedRef<FExtender> Extender = MakeShared<FExtender>();

	for (UObject* EditingObject : EditingObjects)
	{
		UBlueprint* Blueprint = Cast<UBlueprint>(EditingObject);
		if (!Blueprint || !IsContainerCandidate(Blueprint))
		{
			continue;
		}

		// "Asset" is the default section every asset-editor toolbar registers (RegisterDefaultToolBar), so it is
		// a stable anchor in the Blueprint editor toolbar. The combined FExtender is honored by the ToolMenus
		// toolbar generation via this hook name.
		Extender->AddToolBarExtension(
			FName(TEXT("Asset")),
			EExtensionHook::After,
			CommandList,
			FToolBarExtensionDelegate::CreateStatic(
				&FCrowdyContainerEditorToolbar::FillToolbar,
				TWeakObjectPtr<UBlueprint>(Blueprint)));
		break;
	}

	return Extender;
}

void FCrowdyContainerEditorToolbar::FillToolbar(
	FToolBarBuilder& ToolbarBuilder, TWeakObjectPtr<UBlueprint> WeakBlueprint)
{
	if (!WeakBlueprint.IsValid())
	{
		return;
	}

	ToolbarBuilder.AddComboButton(
		FUIAction(),
		FOnGetContent::CreateStatic(&FCrowdyContainerEditorToolbar::BuildMenu, WeakBlueprint),
		LOCTEXT("CrowdyMenuLabel", "Crowdy SDK"),
		LOCTEXT("CrowdyMenuTip",
			"Mark this Blueprint as a Crowdy Game Model container, so its Server Owned variables are synced as "
			"server-authoritative attributes."),
		FSlateIcon(CrowdyToolbarStyle::StyleSetName, CrowdyToolbarStyle::GameModelIconBrush));
}

TSharedRef<SWidget> FCrowdyContainerEditorToolbar::BuildMenu(TWeakObjectPtr<UBlueprint> WeakBlueprint)
{
	// Keep the menu open after the toggle so a class can be marked and its type name edited in one interaction.
	FMenuBuilder MenuBuilder(/*bCloseAfterSelection*/ false, nullptr);

	MenuBuilder.BeginSection(NAME_None, LOCTEXT("GameModelSection", "Game Model"));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("MarkContainerEntry", "Game Model Class"),
		LOCTEXT("MarkContainerEntryTip",
			"When on, this class's Server Owned variables are discovered and synced as a Game Model container. "
			"Recompile to apply."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakBlueprint]()
			{
				if (UBlueprint* Blueprint = WeakBlueprint.Get())
				{
					CrowdyContainerMarker::SetMarked(Blueprint, !CrowdyContainerMarker::IsMarkedContainer(Blueprint));
				}
			}),
			FCanExecuteAction(),
			FGetActionCheckState::CreateLambda([WeakBlueprint]()
			{
				return CrowdyContainerMarker::IsMarkedContainer(WeakBlueprint.Get())
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})),
		NAME_None,
		EUserInterfaceActionType::ToggleButton);

	MenuBuilder.AddWidget(
		SNew(SEditableTextBox)
		.MinDesiredWidth(160.f)
		.Text_Lambda([WeakBlueprint]()
		{
			return FText::FromString(CrowdyContainerMarker::ResolveContainerTypeName(WeakBlueprint.Get()));
		})
		.OnTextCommitted_Lambda([WeakBlueprint](const FText& NewText, ETextCommit::Type)
		{
			if (UBlueprint* Blueprint = WeakBlueprint.Get())
			{
				CrowdyContainerMarker::SetAuthoredTypeName(Blueprint, NewText.ToString());
			}
		})
		.IsEnabled_Lambda([WeakBlueprint]()
		{
			return CrowdyContainerMarker::IsMarkedContainer(WeakBlueprint.Get());
		})
		.ToolTipText(LOCTEXT("ContainerTypeTip",
			"The server container type name. Defaults to the Blueprint's asset name; change it to pin a stable "
			"name independent of a later asset rename.")),
		LOCTEXT("ContainerTypeLabel", "Container Type"));

	MenuBuilder.AddMenuEntry(
		LOCTEXT("PullOnStartEntry", "Pull Model On Start"),
		LOCTEXT("PullOnStartEntryTip",
			"When on, a container of this class fetches its server state once as soon as it binds. Leave it on for "
			"state that must be correct from the first frame; turn it off when the container is pulled on demand "
			"instead. Later updates are unaffected either way: once bound, a model-changed notification still "
			"re-pulls. Recompile to apply."),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateLambda([WeakBlueprint]()
			{
				if (UBlueprint* Blueprint = WeakBlueprint.Get())
				{
					CrowdyContainerMarker::SetPullModelOnStart(
						Blueprint, !CrowdyContainerMarker::GetPullModelOnStart(Blueprint));
				}
			}),
			FCanExecuteAction::CreateLambda([WeakBlueprint]()
			{
				// Only meaningful on a container, so it greys out on an unmarked class the same way the type
				// name field does.
				return CrowdyContainerMarker::IsMarkedContainer(WeakBlueprint.Get());
			}),
			FGetActionCheckState::CreateLambda([WeakBlueprint]()
			{
				return CrowdyContainerMarker::GetPullModelOnStart(WeakBlueprint.Get())
					? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})),
		NAME_None,
		EUserInterfaceActionType::ToggleButton);

	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

#undef LOCTEXT_NAMESPACE
