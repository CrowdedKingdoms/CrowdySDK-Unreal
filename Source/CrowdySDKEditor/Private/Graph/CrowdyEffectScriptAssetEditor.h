// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"
#include "Toolkits/AssetEditorToolkit.h"

class FExtender;
class FToolBarBuilder;
class IDetailsView;
class SCrowdyEffectCompilePanel;
class SCrowdyEffectScriptEditor;
class UCrowdyEffect;

/**
 * A dedicated window for a Text-sourced Crowdy effect: the multi-line EffectScript editor fills a canvas tab (the way
 * the graph editor fills its canvas), and the rest of the asset (container class, tuning values, compile preview, and
 * the Script / Graph mode switch) lives in a side Details tab. Only a Text-sourced effect opens here; a Graph effect
 * opens the node-graph editor instead. Switching the mode row to Graph reopens the asset in the graph editor, so the
 * two authoring surfaces stay symmetrical.
 */
class FCrowdyEffectScriptAssetEditor : public FAssetEditorToolkit
{
public:
	void InitEditor(EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UCrowdyEffect* InEffect);

	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;

private:
	TSharedRef<SDockTab> SpawnScriptTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnDetailsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnCompileTab(const FSpawnTabArgs& Args);

	// The container-class attributes, declared magnitudes, and named formulas the completion draws on, resolved live
	// from the edited effect (the same sources the details-panel expression editors use).
	TArray<FString> GetAttributeNames() const;
	TArray<FString> GetSourceAttributeNames() const;
	bool IsSourceSchemaDeclared() const;

	// The diagnostics a real compile finds in a body the asset has not committed yet, so the editor underlines an
	// unknown or inconsistently spelled attribute as it is typed instead of only reporting syntax.
	TArray<FCrowdyEffectDiagnostic> DiagnoseBody(const FString& Body) const;
	TArray<FString> GetMagnitudeNames() const;
	TArray<FString> GetFunctionNames() const;

	// Write the committed body back onto the effect (transacted) and refresh the details view so its compile preview
	// reflects the edit; the editor is a plain widget, not a property customization, so nothing else observes the write.
	void OnScriptCommitted(const FString& Text);

	// A "Compile" toolbar button that recompiles the effect and refreshes the details preview and diagnostics on demand.
	void ExtendToolbar();
	void FillToolbar(FToolBarBuilder& ToolbarBuilder);
	void CompileEffect();

	UCrowdyEffect* Effect = nullptr;
	TSharedPtr<IDetailsView> EffectDetailsView;
	TSharedPtr<SCrowdyEffectScriptEditor> ScriptEditor;
	TSharedPtr<SCrowdyEffectCompilePanel> CompilePanel;
};
