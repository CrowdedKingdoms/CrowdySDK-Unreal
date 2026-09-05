// Fill out your copyright notice in the Description page of Project Settings.

#include "Graph/CrowdyEffectAssetEditor.h"

#include "Customizations/SCrowdyEffectCompilePanel.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphUtilities.h"
#include "Editor.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/MultiBox/MultiBoxExtender.h"
#include "GameModel/CrowdyEffectDuplicateFunctionIndex.h"
#include "GraphEditor.h"
#include "GraphEditAction.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IDetailsView.h"
#include "Logging/TokenizedMessage.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "TimerManager.h"
#include "Textures/SlateIcon.h"
#include "WorkspaceMenuStructure.h"
#include "Graph/CrowdyEffectGraph.h"
#include "Graph/CrowdyEffectGraphCompiler.h"
#include "Graph/CrowdyEffectGraphNodes.h"
#include "Graph/CrowdyEffectGraphSchema.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "CrowdyEffectAssetEditor"

namespace
{
	const FName CrowdyEffectGraphAppId(TEXT("CrowdyEffectGraphEditorApp"));
	const FName GraphTabId(TEXT("CrowdyEffectGraph_GraphTab"));
	const FName DetailsTabId(TEXT("CrowdyEffectGraph_DetailsTab"));
	const FName NodeDetailsTabId(TEXT("CrowdyEffectGraph_NodeDetailsTab"));
	const FName CompileTabId(TEXT("CrowdyEffectGraph_CompileTab"));
}

FCrowdyEffectAssetEditor::~FCrowdyEffectAssetEditor()
{
	if (GraphChangedHandle.IsValid() && Graph)
	{
		Graph->RemoveOnGraphChangedHandler(GraphChangedHandle);
	}
}

void FCrowdyEffectAssetEditor::InitEditor(
	EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UCrowdyEffect* InEffect)
{
	Effect = InEffect;
	if (!Effect)
	{
		return;
	}

	// A Graph-sourced effect always edits a real graph. Create one lazily (with our schema and its output node) the
	// first time the editor opens, the way a new material materializes its graph on first open.
	if (!Effect->EffectGraph)
	{
		UCrowdyEffectGraph* NewGraph = NewObject<UCrowdyEffectGraph>(
			Effect, UCrowdyEffectGraph::StaticClass(), NAME_None, RF_Transactional);
		NewGraph->Schema = UCrowdyEffectGraphSchema::StaticClass();
		Effect->EffectGraph = NewGraph;
		NewGraph->GetSchema()->CreateDefaultNodesForGraph(*NewGraph);
		Effect->MarkPackageDirty();
	}
	Graph = Effect->EffectGraph;
	if (!Graph->Schema)
	{
		Graph->Schema = UCrowdyEffectGraphSchema::StaticClass();
	}

	// A graph must always have its single output node. A brand-new graph got one above; a loaded graph that somehow
	// has none (a corrupted or pre-Result asset) gets one restored here, so the editor never opens without an output.
	bool bHasResult = false;
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->IsA<UCrowdyEffectGraphNode_Result>())
		{
			bHasResult = true;
			break;
		}
	}
	if (!bHasResult)
	{
		Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
		Effect->MarkPackageDirty();
	}

	// Keep the effect's derived state fresh on any graph edit; the graph is a separate object, so editing it never
	// fires the effect's own PostEditChangeProperty. The handler is removed in the destructor, since the graph
	// outlives this toolkit while its asset stays loaded.
	GraphChangedHandle = Graph->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateLambda(
		[this](const FEdGraphEditAction&) { OnGraphChanged(); }));

	// Stamp the initial badges so an opened graph shows any existing problems (a fresh graph's empty Result) the moment
	// its node widgets are built.
	RefreshNodeErrorBadges();

	BindGraphCommands();

	FPropertyEditorModule& PropertyModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;

	EffectDetailsView = PropertyModule.CreateDetailView(DetailsArgs);
	EffectDetailsView->SetObject(Effect);

	NodeDetailsView = PropertyModule.CreateDetailView(DetailsArgs);
	NodeDetailsView->SetObject(nullptr);

	ExtendToolbar();

	// The compile readout sits directly under the canvas, so the graph and what it compiles to read together without
	// hunting through the side panel. The layout name is versioned, so an existing saved layout does not suppress the
	// new tab.
	const TSharedRef<FTabManager::FLayout> Layout =
		FTabManager::NewLayout("CrowdyEffectGraphEditor_Layout_v2")
		->AddArea(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
			->Split(
				FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)->SetSizeCoefficient(0.72f)
				->Split(
					FTabManager::NewStack()
					->SetSizeCoefficient(0.7f)
					->AddTab(GraphTabId, ETabState::OpenedTab)
					->SetHideTabWell(true))
				->Split(FTabManager::NewStack()->SetSizeCoefficient(0.3f)->AddTab(CompileTabId, ETabState::OpenedTab)))
			->Split(
				FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)->SetSizeCoefficient(0.28f)
				->Split(FTabManager::NewStack()->SetSizeCoefficient(0.55f)->AddTab(DetailsTabId, ETabState::OpenedTab))
				->Split(FTabManager::NewStack()->SetSizeCoefficient(0.45f)->AddTab(NodeDetailsTabId, ETabState::OpenedTab))));

	FAssetEditorToolkit::InitAssetEditor(
		Mode, InitToolkitHost, CrowdyEffectGraphAppId, Layout,
		/*bCreateDefaultStandaloneMenu*/ true, /*bCreateDefaultToolbar*/ true, Effect);
}

void FCrowdyEffectAssetEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	const TSharedRef<FWorkspaceItem> Group = InTabManager->AddLocalWorkspaceMenuCategory(
		LOCTEXT("WorkspaceMenu", "Crowdy Effect Graph"));

	InTabManager->RegisterTabSpawner(GraphTabId,
		FOnSpawnTab::CreateSP(this, &FCrowdyEffectAssetEditor::SpawnGraphTab))
		.SetDisplayName(LOCTEXT("GraphTab", "Graph"))
		.SetGroup(Group);

	InTabManager->RegisterTabSpawner(DetailsTabId,
		FOnSpawnTab::CreateSP(this, &FCrowdyEffectAssetEditor::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTab", "Details"))
		.SetGroup(Group);

	InTabManager->RegisterTabSpawner(NodeDetailsTabId,
		FOnSpawnTab::CreateSP(this, &FCrowdyEffectAssetEditor::SpawnNodeDetailsTab))
		.SetDisplayName(LOCTEXT("NodeDetailsTab", "Selected Node"))
		.SetGroup(Group);

	InTabManager->RegisterTabSpawner(CompileTabId,
		FOnSpawnTab::CreateSP(this, &FCrowdyEffectAssetEditor::SpawnCompileTab))
		.SetDisplayName(LOCTEXT("CompileTab", "Compile"))
		.SetGroup(Group);
}

void FCrowdyEffectAssetEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);
	InTabManager->UnregisterTabSpawner(GraphTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
	InTabManager->UnregisterTabSpawner(NodeDetailsTabId);
	InTabManager->UnregisterTabSpawner(CompileTabId);
}

FName FCrowdyEffectAssetEditor::GetToolkitFName() const { return FName("CrowdyEffectGraphEditor"); }
FText FCrowdyEffectAssetEditor::GetBaseToolkitName() const { return LOCTEXT("ToolkitName", "Crowdy Effect Graph Editor"); }
FString FCrowdyEffectAssetEditor::GetWorldCentricTabPrefix() const { return LOCTEXT("TabPrefix", "Effect ").ToString(); }
FLinearColor FCrowdyEffectAssetEditor::GetWorldCentricTabColorScale() const { return FLinearColor(0.22f, 0.66f, 0.74f, 0.5f); }

TSharedRef<SDockTab> FCrowdyEffectAssetEditor::SpawnGraphTab(const FSpawnTabArgs& Args)
{
	GraphEditorWidget = CreateGraphEditor();
	return SNew(SDockTab)
		.Label(LOCTEXT("GraphTabLabel", "Graph"))
		[
			GraphEditorWidget.ToSharedRef()
		];
}

TSharedRef<SDockTab> FCrowdyEffectAssetEditor::SpawnDetailsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTabLabel", "Details"))
		[
			EffectDetailsView.ToSharedRef()
		];
}

TSharedRef<SDockTab> FCrowdyEffectAssetEditor::SpawnNodeDetailsTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("NodeDetailsTabLabel", "Selected Node"))
		[
			NodeDetailsView.ToSharedRef()
		];
}

TSharedRef<SDockTab> FCrowdyEffectAssetEditor::SpawnCompileTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("CompileTabLabel", "Compile"))
		[
			SAssignNew(CompilePanel, SCrowdyEffectCompilePanel).Effect(Effect)
		];
}

TSharedRef<SGraphEditor> FCrowdyEffectAssetEditor::CreateGraphEditor()
{
	SGraphEditor::FGraphEditorEvents Events;
	Events.OnSelectionChanged =
		SGraphEditor::FOnSelectionChanged::CreateSP(this, &FCrowdyEffectAssetEditor::OnSelectedNodesChanged);
	Events.OnTextCommitted =
		FOnNodeTextCommitted::CreateSP(this, &FCrowdyEffectAssetEditor::OnNodeTitleCommitted);

	FGraphAppearanceInfo Appearance;
	Appearance.CornerText = LOCTEXT("GraphCorner", "CROWDY EFFECT");

	return SNew(SGraphEditor)
		.AdditionalCommands(GraphCommands)
		.IsEditable(true)
		.Appearance(Appearance)
		.GraphToEdit(Graph)
		.GraphEvents(Events);
}

void FCrowdyEffectAssetEditor::OnSelectedNodesChanged(const TSet<UObject*>& NewSelection)
{
	if (!NodeDetailsView.IsValid())
	{
		return;
	}

	// The node details panel shows a single selected node; nothing (the effect stays in its own tab) for a multi- or
	// empty selection.
	if (NewSelection.Num() == 1)
	{
		NodeDetailsView->SetObject(*NewSelection.CreateConstIterator());
	}
	else
	{
		NodeDetailsView->SetObject(nullptr);
	}
}

void FCrowdyEffectAssetEditor::OnNodeTitleCommitted(
	const FText& NewText, ETextCommit::Type CommitInfo, UEdGraphNode* NodeBeingChanged)
{
	if (NodeBeingChanged)
	{
		const FScopedTransaction Transaction(LOCTEXT("RenameNode", "Rename Effect Graph Node"));
		NodeBeingChanged->Modify();
		NodeBeingChanged->OnRenameNode(NewText.ToString());
	}
}

void FCrowdyEffectAssetEditor::OnGraphChanged()
{
	if (Effect)
	{
		Effect->MarkPackageDirty();
	}

	// Do not compile here: a graph edit action can be reported while the mutation is still in progress (a link before
	// both endpoints are settled), which would compile a half-connected graph and stamp a phantom error. Coalesce to a
	// single refresh next tick, when the graph is settled.
	RequestDeferredRefresh();
}

void FCrowdyEffectAssetEditor::RequestDeferredRefresh()
{
	if (bRefreshPending || !GEditor)
	{
		return;
	}
	bRefreshPending = true;

	TWeakPtr<FCrowdyEffectAssetEditor> WeakThis = SharedThis(this);
	GEditor->GetTimerManager()->SetTimerForNextTick([WeakThis]()
	{
		TSharedPtr<FCrowdyEffectAssetEditor> Pinned = WeakThis.Pin();
		if (!Pinned.IsValid())
		{
			return;
		}
		Pinned->bRefreshPending = false;
		Pinned->ApplyGraphRefresh();
	});
}

void FCrowdyEffectAssetEditor::ApplyGraphRefresh()
{
	RefreshEffectDerivedState();
	RefreshNodeErrorBadges();

	// The effect's details panel (its undeclared-param hints and derived fields) is a separate view that only recomputes
	// on the effect's own property edits. A graph edit changes what the effect compiles to, so rebuild that view too,
	// otherwise it shows a stale snapshot from before the edit.
	if (EffectDetailsView.IsValid())
	{
		EffectDetailsView->ForceRefresh();
	}
	if (CompilePanel.IsValid())
	{
		CompilePanel->Refresh();
	}
}

void FCrowdyEffectAssetEditor::ExtendToolbar()
{
	TSharedPtr<FExtender> ToolbarExtender = MakeShared<FExtender>();
	ToolbarExtender->AddToolBarExtension(
		"Asset", EExtensionHook::After, GraphCommands,
		FToolBarExtensionDelegate::CreateSP(this, &FCrowdyEffectAssetEditor::FillToolbar));
	AddToolbarExtender(ToolbarExtender);
}

void FCrowdyEffectAssetEditor::FillToolbar(FToolBarBuilder& ToolbarBuilder)
{
	ToolbarBuilder.BeginSection("CrowdyEffectCompile");
	ToolbarBuilder.AddToolBarButton(
		FUIAction(FExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::ApplyGraphRefresh)),
		NAME_None,
		LOCTEXT("CompileButton", "Compile"),
		LOCTEXT("CompileButtonTooltip", "Recompile this effect graph and refresh the node error badges and compile preview."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"));
	ToolbarBuilder.EndSection();
}

void FCrowdyEffectAssetEditor::RefreshEffectDerivedState()
{
	if (Effect && Effect->Source == ECrowdyEffectSource::Graph)
	{
		// Catalog-free compile: only bSourceReferenced is read, the fn-callee catalog adds warnings alone, and the
		// invalidation right below would immediately throw away any catalog this compile caused to be built.
		Effect->bRequiresSource = Effect->Compile(ECrowdyEffectFnCatalog::None).bSourceReferenced;

		// A graph edit changes a node, which is a separate object from the effect, so the effect's own
		// PostEditChangeProperty never fires and the catalog's cached asset-registry sweep never sees this write.
		CrowdyEffectDuplicateFunctionIndex::NotifyEffectAuthoringChanged();
	}
}

void FCrowdyEffectAssetEditor::RefreshNodeErrorBadges()
{
	UCrowdyEffectGraph* EffectGraph = Cast<UCrowdyEffectGraph>(Graph);
	if (!EffectGraph)
	{
		return;
	}

	// Clear every node first so a node that was fixed since the last compile loses its badge.
	UEdGraphNode* ResultNode = nullptr;
	for (UEdGraphNode* Node : EffectGraph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		Node->bHasCompilerMessage = false;
		Node->ErrorType = EMessageSeverity::Info;
		Node->ErrorMsg.Reset();
		if (Node->IsA<UCrowdyEffectGraphNode_Result>())
		{
			ResultNode = Node;
		}
	}

	TArray<FCrowdyEffectDiagnostic> Diagnostics;
	TArray<FCrowdyEffectGraphNodeDiagnostic> NodeDiagnostics;
	FCrowdyEffectGraphCompiler::CompileToSpecWithNodeDiagnostics(EffectGraph, Diagnostics, NodeDiagnostics);

	for (const FCrowdyEffectGraphNodeDiagnostic& Diagnostic : NodeDiagnostics)
	{
		// A diagnostic with no owning node (a missing Result, an over-deep chain) shows on the output node, so nothing
		// is silently lost.
		UEdGraphNode* Target = const_cast<UEdGraphNode*>(Diagnostic.Node.Get());
		if (!Target)
		{
			Target = ResultNode;
		}
		if (!Target)
		{
			continue;
		}

		const int32 Severity = Diagnostic.Severity == ECrowdyEffectSeverity::Warning
			? EMessageSeverity::Warning
			: EMessageSeverity::Error;

		// Keep the most severe badge (lower EMessageSeverity is more severe), and accumulate messages so a node with
		// several problems lists them all.
		if (!Target->bHasCompilerMessage || Severity < Target->ErrorType)
		{
			Target->ErrorType = Severity;
		}
		Target->bHasCompilerMessage = true;
		Target->ErrorMsg = Target->ErrorMsg.IsEmpty()
			? Diagnostic.Message
			: Target->ErrorMsg + TEXT("\n") + Diagnostic.Message;
	}
}

void FCrowdyEffectAssetEditor::BindGraphCommands()
{
	GraphCommands = MakeShared<FUICommandList>();
	const FGenericCommands& Generic = FGenericCommands::Get();

	GraphCommands->MapAction(Generic.Delete,
		FExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::DeleteSelectedNodes),
		FCanExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::CanDeleteNodes));
	GraphCommands->MapAction(Generic.Copy,
		FExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::CopySelectedNodes),
		FCanExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::CanCopyNodes));
	GraphCommands->MapAction(Generic.Cut,
		FExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::CutSelectedNodes),
		FCanExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::CanDeleteNodes));
	GraphCommands->MapAction(Generic.Paste,
		FExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::PasteNodes),
		FCanExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::CanPasteNodes));
	GraphCommands->MapAction(Generic.Duplicate,
		FExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::DuplicateSelectedNodes),
		FCanExecuteAction::CreateSP(this, &FCrowdyEffectAssetEditor::CanCopyNodes));
}

void FCrowdyEffectAssetEditor::DeleteSelectedNodes()
{
	if (!GraphEditorWidget.IsValid())
	{
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("DeleteNodes", "Delete Effect Graph Nodes"));
	Graph->Modify();

	const FGraphPanelSelectionSet Selected = GraphEditorWidget->GetSelectedNodes();
	GraphEditorWidget->ClearSelectionSet();

	for (UObject* Object : Selected)
	{
		UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
		if (Node && Node->CanUserDeleteNode())
		{
			Node->Modify();
			Node->DestroyNode();
		}
	}

	OnGraphChanged();
}

bool FCrowdyEffectAssetEditor::CanDeleteNodes() const
{
	if (!GraphEditorWidget.IsValid())
	{
		return false;
	}
	for (UObject* Object : GraphEditorWidget->GetSelectedNodes())
	{
		const UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
		if (Node && Node->CanUserDeleteNode())
		{
			return true;
		}
	}
	return false;
}

void FCrowdyEffectAssetEditor::CopySelectedNodes()
{
	if (!GraphEditorWidget.IsValid())
	{
		return;
	}

	// Export only the nodes that may be duplicated (the Result output node is fixed, so it is never copied).
	FGraphPanelSelectionSet Exportable;
	for (UObject* Object : GraphEditorWidget->GetSelectedNodes())
	{
		if (UEdGraphNode* Node = Cast<UEdGraphNode>(Object))
		{
			if (Node->CanDuplicateNode())
			{
				Node->PrepareForCopying();
				Exportable.Add(Node);
			}
		}
	}

	FString Exported;
	FEdGraphUtilities::ExportNodesToText(Exportable, Exported);
	FPlatformApplicationMisc::ClipboardCopy(*Exported);
}

bool FCrowdyEffectAssetEditor::CanCopyNodes() const
{
	if (!GraphEditorWidget.IsValid())
	{
		return false;
	}
	for (UObject* Object : GraphEditorWidget->GetSelectedNodes())
	{
		const UEdGraphNode* Node = Cast<UEdGraphNode>(Object);
		if (Node && Node->CanDuplicateNode())
		{
			return true;
		}
	}
	return false;
}

void FCrowdyEffectAssetEditor::CutSelectedNodes()
{
	CopySelectedNodes();
	DeleteSelectedNodes();
}

void FCrowdyEffectAssetEditor::PasteNodes()
{
	if (!GraphEditorWidget.IsValid() || !Graph)
	{
		return;
	}

	FString ClipboardText;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardText);

	const FScopedTransaction Transaction(LOCTEXT("PasteNodes", "Paste Effect Graph Nodes"));
	Graph->Modify();
	GraphEditorWidget->ClearSelectionSet();

	TSet<UEdGraphNode*> PastedNodes;
	FEdGraphUtilities::ImportNodesFromText(Graph, ClipboardText, PastedNodes);

	for (auto It = PastedNodes.CreateIterator(); It; ++It)
	{
		UEdGraphNode* Node = *It;

		// A graph has exactly one Result output; drop any pasted copy so it is never duplicated.
		if (Node->IsA<UCrowdyEffectGraphNode_Result>())
		{
			Node->DestroyNode();
			It.RemoveCurrent();
			continue;
		}

		Node->CreateNewGuid();
		Node->NodePosX += 24;
		Node->NodePosY += 24;
		GraphEditorWidget->SetNodeSelection(Node, true);
	}

	OnGraphChanged();
}

bool FCrowdyEffectAssetEditor::CanPasteNodes() const
{
	if (!Graph)
	{
		return false;
	}
	FString ClipboardText;
	FPlatformApplicationMisc::ClipboardPaste(ClipboardText);
	return FEdGraphUtilities::CanImportNodesFromText(Graph, ClipboardText);
}

void FCrowdyEffectAssetEditor::DuplicateSelectedNodes()
{
	CopySelectedNodes();
	PasteNodes();
}

#undef LOCTEXT_NAMESPACE
