// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"

class FExtender;
class FToolBarBuilder;
class IDetailsView;
class SCrowdyEffectCompilePanel;
class SGraphEditor;
class UCrowdyEffect;
class UEdGraph;
class UEdGraphNode;

/**
 * A Material-Editor-style asset editor for a Graph-sourced Crowdy effect: a node-graph canvas plus a details panel
 * for the asset and for the selected node. The graph is the effect's editor-only EffectGraph; it compiles through the
 * same effect-graph compiler the headless tests use, so what the designer wires here is exactly what the effect emits.
 * Only a Graph-sourced effect opens here; Text / Structured effects keep the default property editor.
 */
class FCrowdyEffectAssetEditor : public FAssetEditorToolkit
{
public:
	virtual ~FCrowdyEffectAssetEditor() override;

	void InitEditor(EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UCrowdyEffect* InEffect);

	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;

private:
	TSharedRef<SDockTab> SpawnGraphTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnDetailsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnNodeDetailsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnCompileTab(const FSpawnTabArgs& Args);

	TSharedRef<SGraphEditor> CreateGraphEditor();

	void OnSelectedNodesChanged(const TSet<UObject*>& NewSelection);
	void OnNodeTitleCommitted(const FText& NewText, ETextCommit::Type CommitInfo, UEdGraphNode* NodeBeingChanged);
	void OnGraphChanged();

	// A graph edit action can be reported mid-mutation (a link fires before both pins are settled), so compiling the
	// graph in that same call would flag a transient error on a graph that is valid a moment later. Coalesce every edit
	// in a frame into one refresh on the next tick, by when the mutation has completed. The explicit Compile button
	// runs the same refresh immediately, since a deliberate click means the graph is settled.
	void RequestDeferredRefresh();
	void ApplyGraphRefresh();

	// A "Compile" toolbar button that recompiles the graph and refreshes the node badges and details preview on demand,
	// rather than only reactively on an edit.
	void ExtendToolbar();
	void FillToolbar(FToolBarBuilder& ToolbarBuilder);

	void BindGraphCommands();
	void DeleteSelectedNodes();
	bool CanDeleteNodes() const;
	void CopySelectedNodes();
	bool CanCopyNodes() const;
	void CutSelectedNodes();
	void PasteNodes();
	bool CanPasteNodes() const;
	void DuplicateSelectedNodes();

	// Recompute the effect's bRequiresSource from the current graph so the invoke-time guardrail stays accurate; the
	// graph is a separate object, so editing it never fires the effect's own PostEditChangeProperty.
	void RefreshEffectDerivedState();

	// Recompile the graph and stamp each offending node with the engine's on-node error badge (bHasCompilerMessage /
	// ErrorType / ErrorMsg), clearing every node first so a fixed node loses its badge. A whole-graph problem with no
	// owning node shows on the Result node.
	void RefreshNodeErrorBadges();

	UCrowdyEffect* Effect = nullptr;
	UEdGraph* Graph = nullptr;
	FDelegateHandle GraphChangedHandle;
	bool bRefreshPending = false;

	TSharedPtr<SGraphEditor> GraphEditorWidget;
	TSharedPtr<IDetailsView> EffectDetailsView;
	TSharedPtr<IDetailsView> NodeDetailsView;
	TSharedPtr<SCrowdyEffectCompilePanel> CompilePanel;
	TSharedPtr<FUICommandList> GraphCommands;
};
