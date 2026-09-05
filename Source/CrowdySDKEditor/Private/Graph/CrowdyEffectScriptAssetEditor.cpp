// Fill out your copyright notice in the Description page of Project Settings.

#include "Graph/CrowdyEffectScriptAssetEditor.h"

#include "Customizations/CrowdyEffectPickerOptions.h"
#include "Customizations/SCrowdyEffectCompilePanel.h"
#include "Customizations/SCrowdyEffectScriptEditor.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "GameModel/CrowdyEffectDuplicateFunctionIndex.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBox.h"
#include "WorkspaceMenuStructure.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectScriptAssetEditor"

namespace
{
	const FName CrowdyEffectScriptAppId(TEXT("CrowdyEffectScriptEditorApp"));
	const FName ScriptTabId(TEXT("CrowdyEffectScript_ScriptTab"));
	const FName ScriptDetailsTabId(TEXT("CrowdyEffectScript_DetailsTab"));
	const FName ScriptCompileTabId(TEXT("CrowdyEffectScript_CompileTab"));
}

void FCrowdyEffectScriptAssetEditor::InitEditor(
	EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UCrowdyEffect* InEffect)
{
	Effect = InEffect;
	if (!Effect)
	{
		return;
	}

	FPropertyEditorModule& PropertyModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;

	EffectDetailsView = PropertyModule.CreateDetailView(DetailsArgs);
	EffectDetailsView->SetObject(Effect);

	ExtendToolbar();

	// The script canvas is the primary surface (like the graph canvas), with the compile readout directly under it and
	// the asset's other properties in a side details tab. The layout name is versioned, so an existing saved layout
	// does not suppress the new tab.
	const TSharedRef<FTabManager::FLayout> Layout =
		FTabManager::NewLayout("CrowdyEffectScriptEditor_Layout_v2")
		->AddArea(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
			->Split(
				FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)->SetSizeCoefficient(0.72f)
				->Split(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.7f)
					->AddTab(ScriptTabId, ETabState::OpenedTab)
					->SetHideTabWell(true))
				->Split(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.3f)
					->AddTab(ScriptCompileTabId, ETabState::OpenedTab)))
			->Split(
				FTabManager::NewStack()
				->SetSizeCoefficient(0.28f)
				->AddTab(ScriptDetailsTabId, ETabState::OpenedTab)));

	FAssetEditorToolkit::InitAssetEditor(
		Mode, InitToolkitHost, CrowdyEffectScriptAppId, Layout,
		/*bCreateDefaultStandaloneMenu*/ true, /*bCreateDefaultToolbar*/ true, Effect);
}

void FCrowdyEffectScriptAssetEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	const TSharedRef<FWorkspaceItem> Group = InTabManager->AddLocalWorkspaceMenuCategory(
		LOCTEXT("WorkspaceMenu", "Crowdy Effect Script"));

	InTabManager->RegisterTabSpawner(ScriptTabId,
		FOnSpawnTab::CreateSP(this, &FCrowdyEffectScriptAssetEditor::SpawnScriptTab))
		.SetDisplayName(LOCTEXT("ScriptTab", "Script"))
		.SetGroup(Group);

	InTabManager->RegisterTabSpawner(ScriptDetailsTabId,
		FOnSpawnTab::CreateSP(this, &FCrowdyEffectScriptAssetEditor::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTab", "Details"))
		.SetGroup(Group);

	InTabManager->RegisterTabSpawner(ScriptCompileTabId,
		FOnSpawnTab::CreateSP(this, &FCrowdyEffectScriptAssetEditor::SpawnCompileTab))
		.SetDisplayName(LOCTEXT("CompileTab", "Compile"))
		.SetGroup(Group);
}

void FCrowdyEffectScriptAssetEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);
	InTabManager->UnregisterTabSpawner(ScriptTabId);
	InTabManager->UnregisterTabSpawner(ScriptDetailsTabId);
	InTabManager->UnregisterTabSpawner(ScriptCompileTabId);
}

FName FCrowdyEffectScriptAssetEditor::GetToolkitFName() const { return FName("CrowdyEffectScriptEditor"); }
FText FCrowdyEffectScriptAssetEditor::GetBaseToolkitName() const { return LOCTEXT("ToolkitName", "Crowdy Effect Script Editor"); }
FString FCrowdyEffectScriptAssetEditor::GetWorldCentricTabPrefix() const { return LOCTEXT("TabPrefix", "Effect ").ToString(); }
FLinearColor FCrowdyEffectScriptAssetEditor::GetWorldCentricTabColorScale() const { return FLinearColor(0.22f, 0.66f, 0.74f, 0.5f); }

TSharedRef<SDockTab> FCrowdyEffectScriptAssetEditor::SpawnScriptTab(const FSpawnTabArgs& Args)
{
	const FString Initial = Effect ? Effect->EffectScript : FString();

	return SNew(SDockTab)
		.Label(LOCTEXT("ScriptTabLabel", "Script"))
		[
			SNew(SBox)
			.Padding(8.f)
			[
				SAssignNew(ScriptEditor, SCrowdyEffectScriptEditor)
				.InitialText(Initial)
				.HintText(LOCTEXT("ScriptHint", "e.g. self.hp = self.hp - $damage"))
				.AttributeProvider(FCrowdyExpressionAttributeProvider::CreateSP(
					this, &FCrowdyEffectScriptAssetEditor::GetAttributeNames))
				.SourceAttributeProvider(FCrowdyExpressionAttributeProvider::CreateSP(
					this, &FCrowdyEffectScriptAssetEditor::GetSourceAttributeNames))
				.SourceSchemaDeclared(TAttribute<bool>::CreateSP(
					this, &FCrowdyEffectScriptAssetEditor::IsSourceSchemaDeclared))
				.SemanticDiagnosticProvider(FCrowdyEffectBodyDiagnosticProvider::CreateSP(
					this, &FCrowdyEffectScriptAssetEditor::DiagnoseBody))
				.Magnitudes(GetMagnitudeNames())
				.Functions(GetFunctionNames())
				.OnTextCommitted(FCrowdyExpressionTextEvent::CreateSP(
					this, &FCrowdyEffectScriptAssetEditor::OnScriptCommitted))
			]
		];
}

TSharedRef<SDockTab> FCrowdyEffectScriptAssetEditor::SpawnDetailsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTabLabel", "Details"))
		[
			EffectDetailsView.ToSharedRef()
		];
}

TSharedRef<SDockTab> FCrowdyEffectScriptAssetEditor::SpawnCompileTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("CompileTabLabel", "Compile"))
		[
			SAssignNew(CompilePanel, SCrowdyEffectCompilePanel).Effect(Effect)
		];
}

TArray<FString> FCrowdyEffectScriptAssetEditor::GetAttributeNames() const
{
	TArray<FString> Names;
	const UClass* Class = Effect ? Effect->ContainerClass.LoadSynchronous() : nullptr;
	for (const FCrowdyAttributeDef& Def : CrowdyEffectPickerOptions::AttributesForClass(Class))
	{
		Names.Add(CrowdyEffectPickerOptions::AttributeNameForScriptCompletion(Def));
	}
	return Names;
}

bool FCrowdyEffectScriptAssetEditor::IsSourceSchemaDeclared() const
{
	return Effect && !Effect->SourceContainerType.TrimStartAndEnd().IsEmpty();
}

TArray<FString> FCrowdyEffectScriptAssetEditor::GetSourceAttributeNames() const
{
	TArray<FString> Names;
	if (!Effect)
	{
		return Names;
	}
	// Resolved through the same registry lookup the graph picker uses, so both authoring surfaces agree on what a
	// declared type name means. A name that resolves to nothing yields an empty list on purpose: the caller reads that
	// as "offer nothing", and offering the target's attributes instead is exactly the wrong suggestion.
	const UClass* Class = FCrowdyAttributeRegistry::FindContainerClassByTypeName(
		Effect->SourceContainerType.TrimStartAndEnd());
	for (const FCrowdyAttributeDef& Def : CrowdyEffectPickerOptions::AttributesForClass(Class))
	{
		Names.Add(CrowdyEffectPickerOptions::AttributeNameForScriptCompletion(Def));
	}
	return Names;
}

TArray<FCrowdyEffectDiagnostic> FCrowdyEffectScriptAssetEditor::DiagnoseBody(const FString& Body) const
{
	return Effect ? Effect->DiagnoseScriptBody(Body) : TArray<FCrowdyEffectDiagnostic>();
}

TArray<FString> FCrowdyEffectScriptAssetEditor::GetMagnitudeNames() const
{
	TArray<FString> Names;
	if (Effect)
	{
		for (const FCrowdyEffectMagnitude& Magnitude : Effect->Magnitudes)
		{
			Names.Add(Magnitude.Name);
		}
	}
	return Names;
}

TArray<FString> FCrowdyEffectScriptAssetEditor::GetFunctionNames() const
{
	// Named formulas (the retired Structured picker's fn:<name>() targets) are gone; there is currently no other
	// source of fn: autocomplete names for the Text surface (a fn: call to another effect's function is validated
	// at compile time against the project's function catalog, not offered here).
	return {};
}

void FCrowdyEffectScriptAssetEditor::OnScriptCommitted(const FString& Text)
{
	if (!Effect || Effect->EffectScript == Text)
	{
		return;
	}

	{
		const FScopedTransaction Transaction(LOCTEXT("EditScriptTx", "Edit EffectScript"));
		Effect->Modify();
		Effect->EffectScript = Text;

		// Editing the body here bypasses the property system, so PostEditChangeProperty (the usual site) never fires to
		// recompute the persisted "needs a Source" guardrail. Mirror the graph toolkit's RefreshEffectDerivedState, or
		// a body edited from source.<attr> to a self-only body would stay bRequiresSource==true and be wrongly rejected
		// at invoke time (the flag is serialized + cooked). Catalog-free: only bSourceReferenced is read, and the
		// invalidation right below would immediately discard any catalog this compile caused to be built.
		Effect->bRequiresSource = Effect->Compile(ECrowdyEffectFnCatalog::None).bSourceReferenced;

		// Same reason: the property system never sees this write, so the fn: callee catalog would otherwise keep
		// answering with what this effect declared before the edit.
		CrowdyEffectDuplicateFunctionIndex::NotifyEffectAuthoringChanged();
	}

	// The body is written straight onto the effect (not through a property handle), so refresh the details view to
	// recompute its compile preview and undeclared-magnitude hints from the new text.
	if (EffectDetailsView.IsValid())
	{
		EffectDetailsView->ForceRefresh();
	}
	if (CompilePanel.IsValid())
	{
		CompilePanel->Refresh();
	}
}

void FCrowdyEffectScriptAssetEditor::ExtendToolbar()
{
	TSharedPtr<FExtender> ToolbarExtender = MakeShared<FExtender>();
	ToolbarExtender->AddToolBarExtension(
		"Asset", EExtensionHook::After, GetToolkitCommands(),
		FToolBarExtensionDelegate::CreateSP(this, &FCrowdyEffectScriptAssetEditor::FillToolbar));
	AddToolbarExtender(ToolbarExtender);
}

void FCrowdyEffectScriptAssetEditor::FillToolbar(FToolBarBuilder& ToolbarBuilder)
{
	ToolbarBuilder.BeginSection("CrowdyEffectCompile");
	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FCrowdyEffectScriptAssetEditor::CompileEffect)),
		NAME_None,
		LOCTEXT("CompileButton", "Compile"),
		LOCTEXT("CompileButtonTooltip", "Recompile this effect and refresh the compile preview and diagnostics."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"));
	ToolbarBuilder.EndSection();
}

void FCrowdyEffectScriptAssetEditor::CompileEffect()
{
	if (!Effect)
	{
		return;
	}

	// Editing the body bypasses the property system, so recompute the persisted "needs a Source" guardrail here (as
	// OnScriptCommitted does), then rebuild the details view so its compile preview and diagnostics reflect the current
	// body. Clicking the toolbar defocuses the script box first, which commits any in-progress edit before this runs.
	// Catalog-free: only bSourceReferenced is read, and the invalidation right below would immediately discard any
	// catalog this compile caused to be built.
	Effect->bRequiresSource = Effect->Compile(ECrowdyEffectFnCatalog::None).bSourceReferenced;

	// The property system never sees a body committed this way, so drop the cached catalog answer here too (cheap
	// and idempotent if OnScriptCommitted already dropped it for this same edit).
	CrowdyEffectDuplicateFunctionIndex::NotifyEffectAuthoringChanged();
	if (EffectDetailsView.IsValid())
	{
		EffectDetailsView->ForceRefresh();
	}
	if (CompilePanel.IsValid())
	{
		CompilePanel->Refresh();
	}
}

#undef LOCTEXT_NAMESPACE
