// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class SVerticalBox;

// Game-model pane: author the design-time schema the runtime consumes. Master/detail over container
// types (and their properties), sandboxed functions, feature keys, tier-feature grants, the session
// policy, and a bulk JSON seed. Game plane, so it needs a game-capable token.
class SCrowdyGameModelView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyGameModelView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyGameModelView() override;

private:
	FReply OnRefreshClicked();
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

	// Slice 2: structured parameters editor.
	void RebuildParamsRows();
	TSharedRef<SWidget> MakeParamRow(TSharedPtr<FStudioFunctionParam> Param);

	FReply OnAddMutationClicked();

	// Slice 3: structured mutations editor.
	void RebuildMutationRows();
	void RebuildTargetOptions();
	TSharedRef<SWidget> MakeMutationRow(TSharedPtr<FStudioFunctionMutation> Mutation);

	// Slice 4: guided invoke-policy builder. A flat list of requirement rows joined by one top-level
	// connector (and/or), with an "Edit as JSON" escape hatch for nested or unrecognized policies.
	FReply OnAddPolicyRuleClicked();
	FReply OnTogglePolicyJsonClicked();
	void RebuildPolicyRows();
	TSharedRef<SWidget> MakePolicyRow(TSharedPtr<FStudioPolicyRule> Rule);
	TSharedRef<SWidget> MakePolicyRuleField(TSharedPtr<FStudioPolicyRule> Rule);

	// Slice 5: expression help. Cheat-sheet popover (operators, builtins, bound-type properties) shown
	// from the return-expression row; the warnings panel surfaces the server's static-analysis notes.
	TSharedRef<SWidget> MakeExpressionCheatSheet();

	void OnTypeSelectionChanged(TSharedPtr<FStudioContainerType> Type, ESelectInfo::Type);
	void OnFunctionSelectionChanged(TSharedPtr<FStudioFunction> Function, ESelectInfo::Type);

	void HandleContainerTypesChanged();
	void HandlePropertyDefsChanged();
	void HandleFunctionsChanged();
	void HandleFeaturesChanged();
	void HandleTierFeaturesChanged();
	void HandleAccessTiersChanged();
	void HandleRuntimePermissionsChanged();

	// B3-2: live runtime container browser (read-only). Lists the runtime's instantiated containers and
	// shows the selected one's visible property values.
	FReply OnRefreshContainersClicked();
	void HandleContainersChanged();
	void HandleContainerStateChanged();
	void OnContainerSelectionChanged(TSharedPtr<FStudioContainer> Container, ESelectInfo::Type SelectInfo);
	TSharedRef<ITableRow> MakeContainerRow(TSharedPtr<FStudioContainer> Container, const TSharedRef<STableViewBase>& OwnerTable);

	FText GetSelectedTypeLabel() const;
	FText GetPolicyLabel() const;

	FString SelectedTypeName() const;

	TSharedRef<ITableRow> MakeTypeRow(TSharedPtr<FStudioContainerType> Type, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakePropertyRow(TSharedPtr<FStudioPropertyDef> Def, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakeFunctionRow(TSharedPtr<FStudioFunction> Function, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakeFeatureRow(TSharedPtr<FStudioAppFeature> Feature, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakeTierFeatureRow(TSharedPtr<FStudioTierFeature> Grant, const TSharedRef<STableViewBase>& OwnerTable);

	TSharedPtr<FCrowdyStudioController> Controller;

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
	// Slice 6a: tier-feature grant tier picker, over the controller's read-only access-tier list.
	TSharedPtr<SComboBox<TSharedPtr<FStudioAccessTier>>> TierComboBox;
	TSharedPtr<FStudioAccessTier> SelectedTier;
	TSharedPtr<SEditableTextBox> ParticipantRoleBox;
	TSharedPtr<SMultiLineEditableTextBox> SeedBox;

	// Fixed-choice fields, backed by segmented controls (their setters write these).
	FString TypeInstantiableBy = TEXT("member");
	FString TypeDefaultVis = TEXT("public");
	FString PropValueType = TEXT("int");
	FString PropVis = TEXT("public");
	FString PropWritable = TEXT("function");
	FString FnInvokeScope = TEXT("player");
	FString SessionCreationPolicy = TEXT("admin");

	// Slice 2: structured parameters editor working state. EditParams is the working copy; the rows
	// edit each entry in place through its shared pointer, and it serializes via ParamsToJson on save.
	TArray<TSharedPtr<FStudioFunctionParam>> EditParams;
	TSharedPtr<SVerticalBox> ParamsRows;
	TArray<TSharedPtr<FString>> ValueTypeOptions;

	// Slice 3: structured mutations editor working state. EditMutations is the working copy; the
	// rows edit it in place and MutationsToJson serializes it on save. TargetOptions is "self" plus
	// the container types; PropertyOptions is the loaded container type's keys (combo suggestions).
	TArray<TSharedPtr<FStudioFunctionMutation>> EditMutations;
	TSharedPtr<SVerticalBox> MutationRows;
	TArray<TSharedPtr<FString>> TargetOptions;
	TArray<TSharedPtr<FString>> PropertyOptions;

	// Slice 4: invoke-policy builder working state. EditPolicy is the flat requirement list (edited in
	// place through each row's shared pointer); PolicyConnector is the top-level and/or. When a loaded
	// policy is nested or unrecognized, bPolicyRawMode keeps the raw JSON box (FnPolicyBox) instead, and
	// bPolicyNotRepresentable drives the "too advanced for the builder" note. The option arrays back the
	// per-row combos: rule types (fixed), feature keys (from GetFeatures), grid keys (the 6b catalog).
	TArray<TSharedPtr<FStudioPolicyRule>> EditPolicy;
	TSharedPtr<SVerticalBox> PolicyRows;
	FString PolicyConnector = TEXT("and");
	bool bPolicyRawMode = false;
	bool bPolicyNotRepresentable = false;
	TArray<TSharedPtr<FString>> PolicyTypeOptions;
	TArray<TSharedPtr<FString>> PolicyFeatureOptions;
	TArray<TSharedPtr<FString>> PolicyGridKeyOptions;

	// Slice 5: pre-joined static-analysis warnings for the selected function; empty hides the panel.
	FString FnWarningsText;

	// B3-2: live runtime container browser working state.
	TSharedPtr<SListView<TSharedPtr<FStudioContainer>>> ContainerListView;
	TSharedPtr<SEditableTextBox> ContainerTypeFilterBox;
	TSharedPtr<SEditableTextBox> ContainerSessionFilterBox;
	TSharedPtr<SMultiLineEditableTextBox> ContainerStateBox;
};
