// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class IDetailsView;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class SVerticalBox;
struct FPropertyChangedEvent;

// Advanced Game Model authoring: the server's own nouns, edited directly against the live app.
// Container types (and their properties), sandboxed functions, feature keys, tier-feature grants,
// the session policy, a bulk JSON seed, and the Deploy Game Kit flow. Game plane, so it needs a
// game-capable token.
//
// This tab offers everything the server allows, with each consequence written on the control that
// carries it. Deleting in bulk, and deciding what a delete would cost before it runs, belong to the
// Models tab's review instead.
class SCrowdyGameModelAdvancedTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyGameModelAdvancedTab) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyGameModelAdvancedTab() override;

private:
	// The app every write and delete on this tab is aimed at: the one these editors were filled from, falling back
	// to the live selection only when nothing has been loaded. Aiming at the live selection instead would make the
	// controller's app check compare a value to itself, so it could never refuse anything.
	int64 EditorTargetAppId() const;

	FReply OnSaveTypeClicked();
	FReply OnSavePropertyClicked();
	FReply OnSaveFunctionClicked();
	FReply OnDeleteFunctionClicked();
	FReply OnDefineFeatureClicked();
	FReply OnGrantTierFeatureClicked();
	FReply OnRevokeTierFeatureClicked();
	FReply OnSetPolicyClicked();
	FReply OnSeedClicked();
	FReply OnAddParamClicked();

	// Deploy Kit card: edit the Studio-owned transient kit config inline (no asset), preview its emitted artifact
	// counts, and deploy it to the live server (confirm-gated). OnKitDetailsChanged refreshes the preview when a
	// layer or its options change.
	void OnKitDetailsChanged(const FPropertyChangedEvent& Event);
	FReply OnDeployKitClicked();
	// Recompute the cached preview text. The emit is not free, so it runs when the config changes, not per paint;
	// GetKitPreviewText just returns the cache.
	void UpdateKitPreviewText();
	FText GetKitPreviewText() const;
	FText GetKitDeployStatusText() const;

	// Structured parameters editor.
	void RebuildParamsRows();
	TSharedRef<SWidget> MakeParamRow(TSharedPtr<FStudioFunctionParam> Param);

	FReply OnAddMutationClicked();

	// Structured mutations editor.
	void RebuildMutationRows();
	void RebuildTargetOptions();
	TSharedRef<SWidget> MakeMutationRow(TSharedPtr<FStudioFunctionMutation> Mutation);

	// Guided invoke-policy builder. A flat list of requirement rows joined by one top-level
	// connector (and/or), with an "Edit as JSON" escape hatch for nested or unrecognized policies.
	FReply OnAddPolicyRuleClicked();
	FReply OnTogglePolicyJsonClicked();
	void RebuildPolicyRows();
	TSharedRef<SWidget> MakePolicyRow(TSharedPtr<FStudioPolicyRule> Rule);
	TSharedRef<SWidget> MakePolicyRuleField(TSharedPtr<FStudioPolicyRule> Rule);

	// Expression help. Cheat-sheet popover (operators, builtins, bound-type properties) shown
	// from the return-expression row; the warnings panel surfaces the server's static-analysis notes.
	TSharedRef<SWidget> MakeExpressionCheatSheet();

	void OnTypeSelectionChanged(TSharedPtr<FStudioContainerType> Type, ESelectInfo::Type);
	void OnFunctionSelectionChanged(TSharedPtr<FStudioFunction> Function, ESelectInfo::Type);

	// The active app changed and its token was minted.
	void HandleAppChanged();
	// Notice that the panes below describe an app other than the selected one, and empty them if so. Called both
	// from the app-changed announcement and from the list refresh the switch itself triggers, because the
	// announcement waits on the new app's token being minted and a mint can fail, leaving the panes describing the
	// previous app with no announcement ever made. Comparing the app id makes running it twice a no-op.
	void SyncEditorAppScope();
	// Empty everything this tab keeps for itself. The lists are the controller's and it has already emptied them;
	// what survives an app switch is exactly what is held here.
	void ClearAppScopedEditors();

	void HandleContainerTypesChanged();
	void HandlePropertyDefsChanged();
	void HandleFunctionsChanged();
	void HandleFeaturesChanged();
	void HandleTierFeaturesChanged();
	void HandleAccessTiersChanged();
	void HandleRuntimePermissionsChanged();

	FText GetSelectedTypeLabel() const;
	FText GetPolicyLabel() const;

	FString SelectedTypeName() const;

	TSharedRef<ITableRow> MakeTypeRow(TSharedPtr<FStudioContainerType> Type, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakePropertyRow(TSharedPtr<FStudioPropertyDef> Def, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakeFunctionRow(TSharedPtr<FStudioFunction> Function, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakeFeatureRow(TSharedPtr<FStudioAppFeature> Feature, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakeTierFeatureRow(TSharedPtr<FStudioTierFeature> Grant, const TSharedRef<STableViewBase>& OwnerTable);

	TSharedPtr<FCrowdyStudioController> Controller;

	// The app the panes below describe. A container type or a function usually carries the SAME name in a
	// development and a production app, since both are deployed from the same assets, so a write must be aimed at
	// the app its subject was read from rather than at whatever happens to be selected when the button is pressed.
	// Zero means nothing has been loaded for an app yet.
	int64 EditorAppId = 0;

	TSharedPtr<SListView<TSharedPtr<FStudioContainerType>>> TypeListView;
	TSharedPtr<SListView<TSharedPtr<FStudioPropertyDef>>> PropertyListView;
	TSharedPtr<SListView<TSharedPtr<FStudioFunction>>> FunctionListView;
	TSharedPtr<SListView<TSharedPtr<FStudioAppFeature>>> FeatureListView;
	TSharedPtr<SListView<TSharedPtr<FStudioTierFeature>>> TierFeatureListView;

	TSharedPtr<SEditableTextBox> TypeNameBox, DisplayNameBox, TypeDescBox;
	TSharedPtr<SEditableTextBox> PropKeyBox, PropDefaultBox, PropDescBox;
	TSharedPtr<SEditableTextBox> FnNameBox, FnTypeBox, FnDescBox, FnReturnTypeBox, FnReturnExprBox;
	TSharedPtr<SMultiLineEditableTextBox> FnPolicyBox;
	TSharedPtr<SEditableTextBox> FeatureKeyBox, FeatureDescBox;
	TSharedPtr<SEditableTextBox> TierFeatureKeyBox;
	// Tier-feature grant tier picker, over the controller's read-only access-tier list.
	TSharedPtr<SComboBox<TSharedPtr<FStudioAccessTier>>> TierComboBox;
	TSharedPtr<FStudioAccessTier> SelectedTier;
	TSharedPtr<SEditableTextBox> ParticipantRoleBox;
	TSharedPtr<SMultiLineEditableTextBox> SeedBox;

	// Fixed-choice fields, backed by segmented controls (their setters write these).
	FString TypeInstantiableBy = TEXT("member");
	FString TypeDefaultVis = TEXT("public");
	FString TypeScope = TEXT("session");
	FString PropValueType = TEXT("int");
	FString PropVis = TEXT("public");
	FString PropWritable = TEXT("function");
	FString FnInvokeScope = TEXT("player");
	FString SessionCreationPolicy = TEXT("admin");

	// Structured parameters editor working state. EditParams is the working copy; the rows
	// edit each entry in place through its shared pointer, and it serializes via ParamsToJson on save.
	TArray<TSharedPtr<FStudioFunctionParam>> EditParams;
	TSharedPtr<SVerticalBox> ParamsRows;
	TArray<TSharedPtr<FString>> ValueTypeOptions;

	// Structured mutations editor working state. EditMutations is the working copy; the
	// rows edit it in place and MutationsToJson serializes it on save. TargetOptions is "self" plus
	// the container types; PropertyOptions is the loaded container type's keys (combo suggestions).
	TArray<TSharedPtr<FStudioFunctionMutation>> EditMutations;
	TSharedPtr<SVerticalBox> MutationRows;
	TArray<TSharedPtr<FString>> TargetOptions;
	TArray<TSharedPtr<FString>> PropertyOptions;

	// Invoke-policy builder working state. EditPolicy is the flat requirement list (edited in
	// place through each row's shared pointer); PolicyConnector is the top-level and/or. When a loaded
	// policy is nested or unrecognized, bPolicyRawMode keeps the raw JSON box (FnPolicyBox) instead, and
	// bPolicyNotRepresentable drives the "too advanced for the builder" note. The option arrays back the
	// per-row combos: rule types (fixed), feature keys (from GetFeatures), grid keys (the permission catalog).
	TArray<TSharedPtr<FStudioPolicyRule>> EditPolicy;
	TSharedPtr<SVerticalBox> PolicyRows;
	FString PolicyConnector = TEXT("and");
	bool bPolicyRawMode = false;
	bool bPolicyNotRepresentable = false;
	TArray<TSharedPtr<FString>> PolicyTypeOptions;
	TArray<TSharedPtr<FString>> PolicyFeatureOptions;
	TArray<TSharedPtr<FString>> PolicyGridKeyOptions;

	// Pre-joined static-analysis warnings for the selected function; empty hides the panel.
	FString FnWarningsText;

	// Deploy Kit card working state: the inline details editor for the Studio-owned transient kit config, the
	// cached emit preview, and the last deploy's status line (updated from the async deploy callback).
	TSharedPtr<IDetailsView> KitDetailsView;
	FText KitPreviewText;
	FString KitDeployStatus;
	bool bKitDeployInFlight = false;
	// The app the deploy in flight is writing to. Its completion arrives long after the click and carries no app
	// of its own, so this is what decides whether the outcome line still belongs on screen. Zero once the app has
	// changed, which is what makes a completion for the app the user left say nothing at all.
	int64 KitDeployAppId = 0;
};
