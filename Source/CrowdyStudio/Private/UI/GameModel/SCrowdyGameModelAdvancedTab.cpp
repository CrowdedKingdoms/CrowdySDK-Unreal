// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyGameModelAdvancedTab.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IDetailsView.h"
#include "Misc/MessageDialog.h"
#include "Model/FCrowdyStudioController.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Replication/GameModel/Kit/CrowdyGameKitConfig.h"
#include "Replication/GameModel/Kit/CrowdyGameKitPreview.h"
#include "Serialization/JsonSerializer.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioFunctionJson.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/GameModel/CrowdyGameModelWidgets.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

using CrowdyGameModelWidgets::LabeledField;
using CrowdyGameModelWidgets::LabeledChoice;
using CrowdyGameModelWidgets::ListCard;
using CrowdyGameModelWidgets::PolicyTypeLabel;
using CrowdyGameModelWidgets::JoinWarnings;

void SCrowdyGameModelAdvancedTab::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyGameModelAdvancedTab::HandleAppChanged);
		Controller->OnContainerTypesChanged.AddSP(this, &SCrowdyGameModelAdvancedTab::HandleContainerTypesChanged);
		Controller->OnPropertyDefsChanged.AddSP(this, &SCrowdyGameModelAdvancedTab::HandlePropertyDefsChanged);
		Controller->OnFunctionsChanged.AddSP(this, &SCrowdyGameModelAdvancedTab::HandleFunctionsChanged);
		Controller->OnFeaturesChanged.AddSP(this, &SCrowdyGameModelAdvancedTab::HandleFeaturesChanged);
		Controller->OnTierFeaturesChanged.AddSP(this, &SCrowdyGameModelAdvancedTab::HandleTierFeaturesChanged);
		Controller->OnAccessTiersChanged.AddSP(this, &SCrowdyGameModelAdvancedTab::HandleAccessTiersChanged);
		Controller->OnRuntimePermissionsChanged.AddSP(this, &SCrowdyGameModelAdvancedTab::HandleRuntimePermissionsChanged);
	}

	// The app this tab's contents belong to, as of construction. The page is built long after a remembered app was
	// restored and selected, so without this the first list to arrive reads as an app switch and empties editors
	// the user may already have filled in.
	EditorAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;

	// Options for the per-row value-type combo in the parameters editor.
	for (const TCHAR* ValueType : { TEXT("int"), TEXT("float"), TEXT("string"), TEXT("bool"), TEXT("array"), TEXT("object"), TEXT("container_ref") })
	{
		ValueTypeOptions.Add(MakeShared<FString>(ValueType));
	}

	// Options for the per-row rule-type combo in the invoke-policy builder.
	for (const TCHAR* RuleType : { TEXT("owner_of_self"), TEXT("is_current_turn"), TEXT("is_host"), TEXT("is_participant"), TEXT("tier_feature"), TEXT("group_permission"), TEXT("grid_permission"), TEXT("condition") })
	{
		PolicyTypeOptions.Add(MakeShared<FString>(RuleType));
	}

	RebuildTargetOptions();

	// Inline editor for the Studio-owned transient kit config: adding a layer offers the genre presets (Combat,
	// Leaderboards, Guild, Living World) and each layer's options edit in place, so a kit is authored + deployed
	// here without ever creating a content asset.
	{
		FPropertyEditorModule& PropertyEditorModule =
			FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		FDetailsViewArgs DetailsArgs;
		DetailsArgs.bAllowSearch = false;
		DetailsArgs.bShowOptions = false;
		DetailsArgs.bHideSelectionTip = true;
		DetailsArgs.bShowScrollBar = false;
		DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		KitDetailsView = PropertyEditorModule.CreateDetailView(DetailsArgs);
		KitDetailsView->OnFinishedChangingProperties().AddSP(this, &SCrowdyGameModelAdvancedTab::OnKitDetailsChanged);
		if (Controller.IsValid())
		{
			KitDetailsView->SetObject(Controller->GetKitDeployConfig());
		}
	}
	UpdateKitPreviewText();

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	auto Btn = [&Style](const FText& Label, bool bPrimary, FOnClicked OnClick) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.ButtonStyle(&Style, bPrimary ? "Crowdy.Button.Primary" : "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(13.0f, 7.0f))
			.OnClicked(OnClick)
			[ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ];
	};

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(2.0f)
		[
			SNew(SVerticalBox)

			// This tab speaks the server's own nouns on purpose: container type, property definition,
			// container. These controls act directly on the live server for the selected app, and
			// nothing here is reachable from the Models or Live tabs.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle")
					.Text(LOCTEXT("AdvancedBanner", "Advanced uses the server's own vocabulary on purpose: container type, property definition, container. Every control here writes directly to the live server for the app you have selected, and it is only reachable from this tab.")),
					FMargin(16.0f, 14.0f))
			]

			// Container types.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("ContainerTypes", "Container types"), TEXT("cube")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						ListCard(
							SAssignNew(TypeListView, SListView<TSharedPtr<FStudioContainerType>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetContainerTypes() : nullptr)
							.OnGenerateRow(this, &SCrowdyGameModelAdvancedTab::MakeTypeRow)
							.OnSelectionChanged(this, &SCrowdyGameModelAdvancedTab::OnTypeSelectionChanged)
							.SelectionMode(ESelectionMode::Single), 120.0f)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(TypeNameBox, LOCTEXT("TypeName", "Type name"), TEXT("stable key, e.g. weapon")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(DisplayNameBox, LOCTEXT("DisplayName", "Display name"), TEXT("human-friendly name")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(TypeDescBox, LOCTEXT("TypeDesc", "Description"), TEXT("optional")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("InstantiableBy", "Instantiable by"), { TEXT("admin"), TEXT("member"), TEXT("owner") }, { LOCTEXT("InstAdmin", "Admin"), LOCTEXT("InstMember", "Member"), LOCTEXT("InstOwner", "Owner") }, TAttribute<FString>::CreateLambda([this]() { return TypeInstantiableBy; }), [this](const FString& V) { TypeInstantiableBy = V; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("DefaultVis", "Default visibility"), { TEXT("public"), TEXT("owner"), TEXT("hidden") }, { LOCTEXT("DvPublic", "Public"), LOCTEXT("DvOwner", "Owner"), LOCTEXT("DvHidden", "Hidden") }, TAttribute<FString>::CreateLambda([this]() { return TypeDefaultVis; }), [this](const FString& V) { TypeDefaultVis = V; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f).HAlign(HAlign_Right)
					[ Btn(LOCTEXT("SaveType", "Save Container Type"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnSaveTypeClicked)) ],
					FMargin(16.0f, 14.0f))
			]

			// Properties.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[ SNew(STextBlock).Text(this, &SCrowdyGameModelAdvancedTab::GetSelectedTypeLabel).TextStyle(&Style, "Crowdy.Text.Heading") ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						ListCard(
							SAssignNew(PropertyListView, SListView<TSharedPtr<FStudioPropertyDef>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetPropertyDefs() : nullptr)
							.OnGenerateRow(this, &SCrowdyGameModelAdvancedTab::MakePropertyRow)
							.SelectionMode(ESelectionMode::None), 110.0f)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(PropKeyBox, LOCTEXT("PropKey", "Property key"), TEXT("unique within the type")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("PropValueType", "Value type"), { TEXT("int"), TEXT("float"), TEXT("string"), TEXT("bool"), TEXT("array"), TEXT("object"), TEXT("container_ref") }, { LOCTEXT("VtInt", "Int"), LOCTEXT("VtFloat", "Float"), LOCTEXT("VtString", "String"), LOCTEXT("VtBool", "Bool"), LOCTEXT("VtArray", "Array"), LOCTEXT("VtObject", "Object"), LOCTEXT("VtRef", "Ref") }, TAttribute<FString>::CreateLambda([this]() { return PropValueType; }), [this](const FString& V) { PropValueType = V; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(PropDefaultBox, LOCTEXT("PropDefault", "Default (JSON)"), TEXT("optional, JSON-encoded")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("PropVis", "Visibility"), { TEXT("public"), TEXT("owner"), TEXT("hidden") }, { LOCTEXT("PvPublic", "Public"), LOCTEXT("PvOwner", "Owner"), LOCTEXT("PvHidden", "Hidden") }, TAttribute<FString>::CreateLambda([this]() { return PropVis; }), [this](const FString& V) { PropVis = V; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("PropWritable", "Writable"), { TEXT("function"), TEXT("owner"), TEXT("admin") }, { LOCTEXT("WrFunction", "Function"), LOCTEXT("WrOwner", "Owner"), LOCTEXT("WrAdmin", "Admin") }, TAttribute<FString>::CreateLambda([this]() { return PropWritable; }), [this](const FString& V) { PropWritable = V; }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(PropDescBox, LOCTEXT("PropDesc", "Description"), TEXT("optional")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f).HAlign(HAlign_Right)
					[ Btn(LOCTEXT("SaveProperty", "Save Property"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnSavePropertyClicked)) ],
					FMargin(16.0f, 14.0f))
			]

			// Functions.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("Functions", "Functions"), TEXT("wand")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						ListCard(
							SAssignNew(FunctionListView, SListView<TSharedPtr<FStudioFunction>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetFunctions() : nullptr)
							.OnGenerateRow(this, &SCrowdyGameModelAdvancedTab::MakeFunctionRow)
							.OnSelectionChanged(this, &SCrowdyGameModelAdvancedTab::OnFunctionSelectionChanged)
							.SelectionMode(ESelectionMode::Single), 110.0f)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(FnNameBox, LOCTEXT("FnName", "Function name"), TEXT("as the server lists it")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(FnTypeBox, LOCTEXT("FnType", "Bound type"), TEXT("container type, or empty for global")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(FnDescBox, LOCTEXT("FnDesc", "Description"), TEXT("optional")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledField(FnReturnTypeBox, LOCTEXT("FnReturnType", "Return type"), TEXT("optional")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[ LabeledChoice(LOCTEXT("FnInvokeScope", "Invoke scope"), { TEXT("player"), TEXT("server"), TEXT("internal") }, { LOCTEXT("ScPlayer", "Player"), LOCTEXT("ScServer", "Server"), LOCTEXT("ScInternal", "Internal") }, TAttribute<FString>::CreateLambda([this]() { return FnInvokeScope; }), [this](const FString& V) { FnInvokeScope = V; }) ]
					// Return expression with an inline expression-help cheat sheet.
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
						[ SNew(SBox).WidthOverride(140.0f)[ SNew(STextBlock).Text(LOCTEXT("FnReturnExpr", "Return expression")).TextStyle(&Style, "Crowdy.Text.Body") ] ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[ SAssignNew(FnReturnExprBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("FnReturnExprHint", "optional, e.g. self.hp")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SComboButton)
							.ContentPadding(FMargin(8.0f, 3.0f))
							.ToolTipText(LOCTEXT("ExprHelpTip", "Operators, builtins, and properties you can use in expressions"))
							.OnGetMenuContent(FOnGetContent::CreateSP(this, &SCrowdyGameModelAdvancedTab::MakeExpressionCheatSheet))
							.ButtonContent()
							[ SNew(STextBlock).Text(LOCTEXT("ExprHelp", "Expression help")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[ SNew(STextBlock).Text(LOCTEXT("FnParams", "Parameters")).TextStyle(&Style, "Crowdy.Text.Body") ]
						+ SVerticalBox::Slot().AutoHeight()[ SAssignNew(ParamsRows, SVerticalBox) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f).HAlign(HAlign_Left)
						[ Btn(LOCTEXT("AddParam", "+ Add parameter"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnAddParamClicked)) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[ SNew(STextBlock).Text(LOCTEXT("FnMutations", "Mutations")).TextStyle(&Style, "Crowdy.Text.Body") ]
						+ SVerticalBox::Slot().AutoHeight()[ SAssignNew(MutationRows, SVerticalBox) ]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f).HAlign(HAlign_Left)
						[ Btn(LOCTEXT("AddMutation", "+ Add mutation"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnAddMutationClicked)) ]
					]
					// Invoke policy: a guided requirement list, with a raw-JSON escape hatch.
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
							[ SNew(STextBlock).Text(LOCTEXT("FnPolicy", "Invoke policy")).TextStyle(&Style, "Crowdy.Text.Body") ]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[
								SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(10.0f, 4.0f))
								.ToolTipText(LOCTEXT("PolToggleTip", "Switch between the guided builder and the raw rule tree"))
								.OnClicked(FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnTogglePolicyJsonClicked))
								[ SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground())
									.Text_Lambda([this]() { return bPolicyRawMode ? LOCTEXT("PolUseBuilder", "Use builder") : LOCTEXT("PolEditJson", "Edit as JSON"); }) ]
							]
						]

						// Guided builder (hidden in raw mode).
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SVerticalBox)
							.Visibility_Lambda([this]() { return bPolicyRawMode ? EVisibility::Collapsed : EVisibility::Visible; })
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
								[ SNew(STextBlock).Text(LOCTEXT("PolMustSatisfy", "Caller must satisfy")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
								[ SNew(SBox).WidthOverride(150.0f)[ CrowdyStudioWidgets::SegmentedEnum({ TEXT("and"), TEXT("or") }, { LOCTEXT("PolAll", "All"), LOCTEXT("PolAny", "Any") }, TAttribute<FString>::CreateLambda([this]() { return PolicyConnector; }), [this](const FString& V) { PolicyConnector = V; }) ] ]
								+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
								[ SNew(STextBlock).Text(LOCTEXT("PolOfThese", "of these requirements")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
							]
							+ SVerticalBox::Slot().AutoHeight()[ SAssignNew(PolicyRows, SVerticalBox) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f).HAlign(HAlign_Left)
							[ Btn(LOCTEXT("AddPolicyRule", "+ Add requirement"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnAddPolicyRuleClicked)) ]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
							[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("PolNote", "Admins always bypass. No requirements = anyone with app access can call.")) ]
						]

						// Raw JSON escape hatch (shown in raw mode).
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SVerticalBox)
							.Visibility_Lambda([this]() { return bPolicyRawMode ? EVisibility::Visible : EVisibility::Collapsed; })
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SBox).HeightOverride(64.0f)
								[ SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(2.0f))[ SAssignNew(FnPolicyBox, SMultiLineEditableTextBox).HintText(LOCTEXT("FnPolicyRawHint", "authority rule tree as JSON")) ] ]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
							[ SNew(STextBlock).AutoWrapText(true)
								.Visibility_Lambda([this]() { return bPolicyNotRepresentable ? EVisibility::Visible : EVisibility::Collapsed; })
								.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
								.Text(LOCTEXT("PolTooAdvanced", "This policy is too advanced for the builder (nested or unknown rules). Keep editing as JSON.")) ]
						]
					]
					// Static-analysis warnings the server returned for this function. Shown after a
					// save or when a function with warnings is selected; hidden when there are none.
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(SVerticalBox)
						.Visibility_Lambda([this]() { return FnWarningsText.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 3.0f)
						[ SNew(STextBlock).Text(LOCTEXT("FnWarnings", "Static-analysis warnings")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Warning())) ]
						+ SVerticalBox::Slot().AutoHeight()
						[ SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Warning())).Text_Lambda([this]() { return FText::FromString(FnWarningsText); }) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f).HAlign(HAlign_Right)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)[ Btn(LOCTEXT("SaveFunction", "Save Function"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnSaveFunctionClicked)) ]
						// The raw escape hatch, with its consequence written on it. This tab exists to offer
						// everything the server allows; the Models tab is where a delete that would silently undo
						// itself is refused instead.
						+ SHorizontalBox::Slot().AutoWidth()[ Btn(LOCTEXT("DeleteFunction", "Delete on server (recreated by the next Sync)"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnDeleteFunctionClicked)) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Features.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("Features", "Features"), TEXT("check")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						ListCard(
							SAssignNew(FeatureListView, SListView<TSharedPtr<FStudioAppFeature>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetFeatures() : nullptr)
							.OnGenerateRow(this, &SCrowdyGameModelAdvancedTab::MakeFeatureRow)
							.SelectionMode(ESelectionMode::None), 90.0f)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)[ SAssignNew(FeatureKeyBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("FeatureKeyHint", "featureKey")).MinDesiredWidth(160.0f) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ SAssignNew(FeatureDescBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("FeatureDescHint", "description (optional)")) ]
						+ SHorizontalBox::Slot().AutoWidth()[ Btn(LOCTEXT("DefineFeature", "Define"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnDefineFeatureClicked)) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Tier-feature grants.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("TierFeatures", "Tier-feature grants"), TEXT("apps")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						ListCard(
							SAssignNew(TierFeatureListView, SListView<TSharedPtr<FStudioTierFeature>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetTierFeatures() : nullptr)
							.OnGenerateRow(this, &SCrowdyGameModelAdvancedTab::MakeTierFeatureRow)
							.SelectionMode(ESelectionMode::None), 90.0f)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SBox).WidthOverride(170.0f)
							[
								SAssignNew(TierComboBox, SComboBox<TSharedPtr<FStudioAccessTier>>)
								.OptionsSource(Controller.IsValid() ? &Controller->GetAccessTiers() : nullptr)
								.OnGenerateWidget_Lambda([](TSharedPtr<FStudioAccessTier> Tier) { return SNew(STextBlock).Text(FText::FromString(Tier.IsValid() ? FString::Printf(TEXT("%s  (#%lld)"), *Tier->Name, Tier->TierId) : FString())); })
								.OnSelectionChanged_Lambda([this](TSharedPtr<FStudioAccessTier> Tier, ESelectInfo::Type) { SelectedTier = Tier; })
								[ SNew(STextBlock).Text_Lambda([this]() { return SelectedTier.IsValid() ? FText::FromString(SelectedTier->Name) : LOCTEXT("PickTier", "Select tier"); }) ]
							]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ SAssignNew(TierFeatureKeyBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("TierFeatureKeyHint", "featureKey")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)[ Btn(LOCTEXT("GrantTierFeature", "Grant"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnGrantTierFeatureClicked)) ]
						+ SHorizontalBox::Slot().AutoWidth()[ Btn(LOCTEXT("RevokeTierFeature", "Revoke"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnRevokeTierFeatureClicked)) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Session policy.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("GameModelPolicy", "Session policy"), TEXT("config")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(this, &SCrowdyGameModelAdvancedTab::GetPolicyLabel) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ CrowdyStudioWidgets::SegmentedEnum({ TEXT("admin"), TEXT("member"), TEXT("anyone") }, { LOCTEXT("ScpAdmin", "Admin"), LOCTEXT("ScpMember", "Member"), LOCTEXT("ScpAnyone", "Anyone") }, TAttribute<FString>::CreateLambda([this]() { return SessionCreationPolicy; }), [this](const FString& V) { SessionCreationPolicy = V; }) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ SAssignNew(ParticipantRoleBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ParticipantRoleHint", "defaultParticipantRole")) ]
						+ SHorizontalBox::Slot().AutoWidth()[ Btn(LOCTEXT("SetGmPolicy", "Set Policy"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnSetPolicyClicked)) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Bulk seed.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("Seed", "Bulk seed"), TEXT("server")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SBox).HeightOverride(120.0f)
						[ SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(2.0f))[ SAssignNew(SeedBox, SMultiLineEditableTextBox).HintText(LOCTEXT("SeedHint", "SeedGameModelInput JSON (appId is added for you): { \"containerTypes\": [...], \"propertyDefinitions\": [...], \"functions\": [...] }")) ] ]
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[ Btn(LOCTEXT("SeedButton", "Seed Definitions"), false, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnSeedClicked)) ],
					FMargin(16.0f, 14.0f))
			]

			// Deploy Game Kit: author genre layers inline and seed them (types, properties, functions,
			// automations) into this app in one pass. No asset - the kit is configured here and deployed.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("DeployKit", "Deploy Game Kit"), TEXT("server")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true)
						.Text(LOCTEXT("DeployKitHelp", "Add one or more genre layers (Combat, Leaderboards, Guild, Living World), set their options, then Deploy. Deploy seeds the container types, property definitions, functions, and automations the kit defines into this app. It writes server schema and starter data; it never deletes."))
					]
					// Inline layer editor: add a genre layer and configure it in place, no asset needed.
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(2.0f))
						[ KitDetailsView.ToSharedRef() ]
					]
					// Live preview of the configured kit's emitted artifact counts (or a validation error).
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true)
						.Text(this, &SCrowdyGameModelAdvancedTab::GetKitPreviewText)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f).HAlign(HAlign_Left)
					[
						SNew(SBox).IsEnabled_Lambda([this]() { return !bKitDeployInFlight; })
						[ Btn(LOCTEXT("DeployKitButton", "Deploy Kit"), true, FOnClicked::CreateSP(this, &SCrowdyGameModelAdvancedTab::OnDeployKitClicked)) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(8.0f))
						[ SNew(STextBlock).AutoWrapText(true).Text(this, &SCrowdyGameModelAdvancedTab::GetKitDeployStatusText) ]
					],
					FMargin(16.0f, 14.0f))
			]
		]
	];
}

SCrowdyGameModelAdvancedTab::~SCrowdyGameModelAdvancedTab()
{
	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.RemoveAll(this);
		Controller->OnContainerTypesChanged.RemoveAll(this);
		Controller->OnPropertyDefsChanged.RemoveAll(this);
		Controller->OnFunctionsChanged.RemoveAll(this);
		Controller->OnFeaturesChanged.RemoveAll(this);
		Controller->OnTierFeaturesChanged.RemoveAll(this);
		Controller->OnAccessTiersChanged.RemoveAll(this);
		Controller->OnRuntimePermissionsChanged.RemoveAll(this);
	}
}

int64 SCrowdyGameModelAdvancedTab::EditorTargetAppId() const
{
	// The app these editors were filled from, not whatever is selected at this instant: reading the selection would
	// agree with itself no matter how stale the boxes had become, and the controller's refusal on a mismatch would
	// never fire. It falls back to the selection only when nothing has been loaded yet, i.e. when the contents were
	// typed rather than picked from a list.
	if (!Controller.IsValid())
	{
		return 0;
	}
	return EditorAppId != 0 ? EditorAppId : Controller->GetSelectedAppId();
}

FReply SCrowdyGameModelAdvancedTab::OnSaveTypeClicked()
{
	if (Controller.IsValid() && TypeNameBox.IsValid())
	{
		Controller->UpsertContainerType(
			TypeNameBox->GetText().ToString(),
			DisplayNameBox->GetText().ToString(),
			TypeDescBox->GetText().ToString(),
			TypeInstantiableBy,
			TypeDefaultVis,
			EditorTargetAppId());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelAdvancedTab::OnSavePropertyClicked()
{
	if (Controller.IsValid() && PropKeyBox.IsValid())
	{
		Controller->UpsertPropertyDef(
			SelectedTypeName(),
			PropKeyBox->GetText().ToString(),
			PropValueType,
			PropDefaultBox->GetText().ToString(),
			PropVis,
			PropWritable,
			PropDescBox->GetText().ToString(),
			EditorTargetAppId());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelAdvancedTab::OnSaveFunctionClicked()
{
	if (Controller.IsValid() && FnNameBox.IsValid())
	{
		// Serialize the parameter rows back to the JSON the server expects.
		TArray<FStudioFunctionParam> Params;
		for (int32 Index = 0; Index < EditParams.Num(); ++Index)
		{
			FStudioFunctionParam P = *EditParams[Index];
			P.SortOrder = Index;
			Params.Add(P);
		}

		TArray<FStudioFunctionMutation> Mutations;
		for (const TSharedPtr<FStudioFunctionMutation>& M : EditMutations)
		{
			if (M.IsValid())
			{
				Mutations.Add(*M);
			}
		}

		// In raw mode the user owns the JSON verbatim; otherwise serialize the guided builder.
		const FString PolicyJson = (bPolicyRawMode && FnPolicyBox.IsValid())
			? FnPolicyBox->GetText().ToString()
			: CrowdyStudioFunctionJson::PolicyToJson(PolicyConnector, EditPolicy);

		Controller->UpsertFunction(
			FnNameBox->GetText().ToString(),
			FnTypeBox->GetText().ToString(),
			FnDescBox->GetText().ToString(),
			FnReturnTypeBox->GetText().ToString(),
			FnInvokeScope,
			FnReturnExprBox->GetText().ToString(),
			CrowdyStudioFunctionJson::ParamsToJson(Params),
			CrowdyStudioFunctionJson::MutationsToJson(Mutations),
			PolicyJson,
			EditorTargetAppId());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelAdvancedTab::OnDeleteFunctionClicked()
{
	if (!Controller.IsValid())
	{
		return FReply::Handled();
	}

	// The target comes from the list that SHOWS the functions, read at the moment of the click, and never from the
	// editor boxes beside it. Those boxes are free text: selecting one function and then typing another model's name
	// into the bound-type field would aim this delete's own status line at a model the caller never picked, and an
	// app-identity check cannot catch that because what has gone wrong is target identity.
	if (!FunctionListView.IsValid() || FunctionListView->GetNumItemsSelected() == 0)
	{
		// The controller's own refusal, so the wording for "nothing to delete" lives in one place.
		Controller->DeleteFunction(FString(), FString(), EditorTargetAppId());
		return FReply::Handled();
	}

	const TArray<TSharedPtr<FStudioFunction>> Selected = FunctionListView->GetSelectedItems();
	if (Selected.Num() == 0 || !Selected[0].IsValid())
	{
		Controller->DeleteFunction(FString(), FString(), EditorTargetAppId());
		return FReply::Handled();
	}

	// This tab's whole purpose is to offer what the server allows, with the consequence written on the control, so
	// the consequence is on the button and there is no second prompt behind it. The delete review on the Models tab
	// is where a marked set is checked, weighed and confirmed; two places asking the same question would mean no
	// single change could prove either of them was doing the asking.
	Controller->DeleteFunction(Selected[0]->ContainerTypeName, Selected[0]->Name, EditorTargetAppId());
	return FReply::Handled();
}

FReply SCrowdyGameModelAdvancedTab::OnDefineFeatureClicked()
{
	if (Controller.IsValid() && FeatureKeyBox.IsValid())
	{
		Controller->DefineFeature(FeatureKeyBox->GetText().ToString(), FeatureDescBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelAdvancedTab::OnGrantTierFeatureClicked()
{
	if (Controller.IsValid() && SelectedTier.IsValid() && TierFeatureKeyBox.IsValid())
	{
		Controller->GrantTierFeature(SelectedTier->TierId, TierFeatureKeyBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelAdvancedTab::OnRevokeTierFeatureClicked()
{
	if (Controller.IsValid() && SelectedTier.IsValid() && TierFeatureKeyBox.IsValid())
	{
		Controller->RevokeTierFeature(SelectedTier->TierId, TierFeatureKeyBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelAdvancedTab::OnSetPolicyClicked()
{
	if (Controller.IsValid() && ParticipantRoleBox.IsValid())
	{
		Controller->SetGameModelPolicy(SessionCreationPolicy, ParticipantRoleBox->GetText().ToString());
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelAdvancedTab::OnSeedClicked()
{
	if (Controller.IsValid() && SeedBox.IsValid())
	{
		Controller->SeedGameModel(SeedBox->GetText().ToString());
	}
	return FReply::Handled();
}

void SCrowdyGameModelAdvancedTab::OnKitDetailsChanged(const FPropertyChangedEvent&)
{
	UpdateKitPreviewText();
}

void SCrowdyGameModelAdvancedTab::UpdateKitPreviewText()
{
	const UCrowdyGameKitConfig* Config = Controller.IsValid() ? Controller->GetKitDeployConfig() : nullptr;
	if (!Config || Config->Layers.Num() == 0)
	{
		KitPreviewText = LOCTEXT("DeployKitPreviewEmpty",
			"Add a genre layer above (Combat, Leaderboards, Guild, or Living World) to configure a kit.");
		return;
	}

	// The preview counts are app-id-independent (the app id only stamps the emitted JSON), so a fixed placeholder is
	// fine here; the real deploy re-emits with the selected app's id.
	const FCrowdyKitPreview Preview = CrowdyKitPreviewLayers(Config->Layers, 1, Config->SessionId);
	if (!Preview.bOk)
	{
		KitPreviewText = FText::FromString(FString::Printf(TEXT("This kit cannot deploy: %s"), *Preview.Error));
		return;
	}
	KitPreviewText = FText::FromString(FString::Printf(
		TEXT("Ready to deploy: %d container type(s), %d property def(s), %d function(s), %d automation(s)."),
		Preview.NumContainerTypes, Preview.NumPropertyDefs, Preview.NumFunctions, Preview.NumAutomations));
}

FText SCrowdyGameModelAdvancedTab::GetKitPreviewText() const
{
	return KitPreviewText;
}

FText SCrowdyGameModelAdvancedTab::GetKitDeployStatusText() const
{
	if (KitDeployStatus.IsEmpty())
	{
		return LOCTEXT("DeployKitStatusIdle",
			"Deploy writes to the live server for the selected app. It needs a session sign-in that can mint an app token.");
	}
	return FText::FromString(KitDeployStatus);
}

FReply SCrowdyGameModelAdvancedTab::OnDeployKitClicked()
{
	if (!Controller.IsValid() || bKitDeployInFlight)
	{
		return FReply::Handled();
	}

	UCrowdyGameKitConfig* Config = Controller->GetKitDeployConfig();
	if (!Config || Config->Layers.Num() == 0)
	{
		KitDeployStatus = TEXT("Add a genre layer to configure a kit before deploying.");
		return FReply::Handled();
	}

	// Validate and get the counts for the confirm before any write.
	const int64 AppId = Controller->GetSelectedAppId();
	if (AppId == 0)
	{
		KitDeployStatus = TEXT("Select an app before deploying a Game Kit.");
		return FReply::Handled();
	}
	const FCrowdyKitPreview Preview = CrowdyKitPreviewLayers(Config->Layers, AppId, Config->SessionId);
	if (!Preview.bOk)
	{
		KitDeployStatus = FString::Printf(TEXT("Cannot deploy: %s"), *Preview.Error);
		return FReply::Handled();
	}

	// Deploying writes live server state, so confirm the exact counts + the app id first.
	const FText Message = FText::Format(
		LOCTEXT("DeployKitConfirm", "Deploy this Game Kit to the live server for app {0}?\n\nThis seeds {1} container type(s), {2} property definition(s), {3} function(s), and {4} automation(s). It writes server schema and starter data; it never deletes existing state."),
		FText::FromString(FString::Printf(TEXT("%lld"), AppId)),
		FText::AsNumber(Preview.NumContainerTypes), FText::AsNumber(Preview.NumPropertyDefs),
		FText::AsNumber(Preview.NumFunctions), FText::AsNumber(Preview.NumAutomations));
	if (FMessageDialog::Open(EAppMsgType::YesNo, Message) != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}

	bKitDeployInFlight = true;
	KitDeployAppId = AppId;
	KitDeployStatus = TEXT("Deploying Game Kit...");

	const TWeakPtr<SWidget> WeakSelf = AsShared();
	Controller->DeployGameKit(Config,
		[this, WeakSelf, AppId](bool /*bOk*/, const FString& DeployMessage)
		{
			if (!WeakSelf.IsValid())
			{
				return;
			}
			bKitDeployInFlight = false;
			// The deploy is a chain of writes against the app it started on, and it can outlive a switch to
			// another app. This line carries no app of its own, so under a different app it would read as that
			// app's outcome and claim a kit is installed somewhere it is not. A deploy the user has navigated
			// away from says nothing rather than saying it under the wrong heading.
			if (AppId != KitDeployAppId)
			{
				return;
			}
			KitDeployStatus = DeployMessage;
		});
	return FReply::Handled();
}

FReply SCrowdyGameModelAdvancedTab::OnAddParamClicked()
{
	const TSharedPtr<FStudioFunctionParam> Param = MakeShared<FStudioFunctionParam>();
	Param->ValueType = TEXT("int");
	Param->bRequired = true;
	EditParams.Add(Param);
	RebuildParamsRows();
	return FReply::Handled();
}

void SCrowdyGameModelAdvancedTab::RebuildParamsRows()
{
	if (!ParamsRows.IsValid())
	{
		return;
	}

	ParamsRows->ClearChildren();
	for (const TSharedPtr<FStudioFunctionParam>& Param : EditParams)
	{
		ParamsRows->AddSlot().AutoHeight().Padding(0.0f, 2.0f)[ MakeParamRow(Param) ];
	}
}

TSharedRef<SWidget> SCrowdyGameModelAdvancedTab::MakeParamRow(TSharedPtr<FStudioFunctionParam> Param)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// Each widget edits the parameter in place through the shared pointer, so the row stays correct
	// even as rows are added or removed above it.
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ParamName", "name"))
			.Text(FText::FromString(Param->Name))
			.OnTextChanged_Lambda([Param](const FText& NewText) { Param->Name = NewText.ToString(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(120.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&ValueTypeOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
				.OnSelectionChanged_Lambda([Param](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Param->ValueType = *Item; } })
				[ SNew(STextBlock).Text_Lambda([Param]() { return FText::FromString(Param->ValueType); }) ]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ParamDefault", "default (JSON, optional)"))
			.Text(FText::FromString(Param->DefaultValueJson))
			.OnTextChanged_Lambda([Param](const FText& NewText) { Param->DefaultValueJson = NewText.ToString(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SCheckBox)
			.IsChecked(Param->bRequired ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
			.OnCheckStateChanged_Lambda([Param](ECheckBoxState State) { Param->bRequired = (State == ECheckBoxState::Checked); })
			[ SNew(STextBlock).Text(LOCTEXT("ParamRequired", "required")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(8.0f, 3.0f))
			.ToolTipText(LOCTEXT("ParamRemoveTip", "Remove this parameter"))
			.OnClicked_Lambda([this, Param]() { EditParams.Remove(Param); RebuildParamsRows(); return FReply::Handled(); })
			[ SNew(STextBlock).Text(LOCTEXT("ParamRemove", "Remove")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
		];
}

FReply SCrowdyGameModelAdvancedTab::OnAddMutationClicked()
{
	const TSharedPtr<FStudioFunctionMutation> Mutation = MakeShared<FStudioFunctionMutation>();
	Mutation->Target = TEXT("self");
	EditMutations.Add(Mutation);
	RebuildMutationRows();
	return FReply::Handled();
}

void SCrowdyGameModelAdvancedTab::RebuildTargetOptions()
{
	TargetOptions.Reset();
	TargetOptions.Add(MakeShared<FString>(TEXT("self")));
	if (Controller.IsValid())
	{
		for (const TSharedPtr<FStudioContainerType>& Type : Controller->GetContainerTypes())
		{
			if (Type.IsValid() && !Type->TypeName.IsEmpty())
			{
				TargetOptions.Add(MakeShared<FString>(Type->TypeName));
			}
		}
	}
}

void SCrowdyGameModelAdvancedTab::RebuildMutationRows()
{
	if (!MutationRows.IsValid())
	{
		return;
	}

	MutationRows->ClearChildren();
	for (const TSharedPtr<FStudioFunctionMutation>& Mutation : EditMutations)
	{
		MutationRows->AddSlot().AutoHeight().Padding(0.0f, 2.0f)[ MakeMutationRow(Mutation) ];
	}
}

TSharedRef<SWidget> SCrowdyGameModelAdvancedTab::MakeMutationRow(TSharedPtr<FStudioFunctionMutation> Mutation)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// Target is "self" (the function's own container) or another container type; property is a key
	// suggested from the loaded container type. Each widget edits the mutation through the shared
	// pointer, so rows stay correct as others are added or removed.
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(110.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&TargetOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
				.OnSelectionChanged_Lambda([Mutation](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Mutation->Target = *Item; } })
				[ SNew(STextBlock).Text_Lambda([Mutation]() { return FText::FromString(Mutation->Target.IsEmpty() ? FString(TEXT("self")) : Mutation->Target); }) ]
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(140.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&PropertyOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
				.OnSelectionChanged_Lambda([Mutation](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Mutation->Property = *Item; } })
				[ SNew(STextBlock).Text_Lambda([Mutation]() { return FText::FromString(Mutation->Property.IsEmpty() ? FString(TEXT("property")) : Mutation->Property); }) ]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("MutExpr", "expression, e.g. hp - $amount"))
			.Text(FText::FromString(Mutation->Expression))
			.OnTextChanged_Lambda([Mutation](const FText& NewText) { Mutation->Expression = NewText.ToString(); })
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(8.0f, 3.0f))
			.ToolTipText(LOCTEXT("MutRemoveTip", "Remove this mutation"))
			.OnClicked_Lambda([this, Mutation]() { EditMutations.Remove(Mutation); RebuildMutationRows(); return FReply::Handled(); })
			[ SNew(STextBlock).Text(LOCTEXT("MutRemove", "Remove")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
		];
}

FReply SCrowdyGameModelAdvancedTab::OnAddPolicyRuleClicked()
{
	const TSharedPtr<FStudioPolicyRule> Rule = MakeShared<FStudioPolicyRule>();
	Rule->Type = TEXT("owner_of_self");
	EditPolicy.Add(Rule);
	RebuildPolicyRows();
	return FReply::Handled();
}

FReply SCrowdyGameModelAdvancedTab::OnTogglePolicyJsonClicked()
{
	if (bPolicyRawMode)
	{
		// Raw -> builder: only switch if the JSON is something the builder can represent.
		TArray<TSharedPtr<FStudioPolicyRule>> ParsedRules;
		FString ParsedConnector;
		if (FnPolicyBox.IsValid() && CrowdyStudioFunctionJson::ParsePolicyJson(FnPolicyBox->GetText().ToString(), ParsedRules, ParsedConnector))
		{
			EditPolicy = ParsedRules;
			PolicyConnector = ParsedConnector;
			bPolicyRawMode = false;
			bPolicyNotRepresentable = false;
			RebuildPolicyRows();
		}
		else
		{
			bPolicyNotRepresentable = true;
		}
	}
	else
	{
		// Builder -> raw: serialize the current builder state into the box for hand-editing.
		if (FnPolicyBox.IsValid())
		{
			FnPolicyBox->SetText(FText::FromString(CrowdyStudioFunctionJson::PolicyToJson(PolicyConnector, EditPolicy)));
		}
		bPolicyRawMode = true;
		bPolicyNotRepresentable = false;
	}
	return FReply::Handled();
}

void SCrowdyGameModelAdvancedTab::RebuildPolicyRows()
{
	if (!PolicyRows.IsValid())
	{
		return;
	}

	PolicyRows->ClearChildren();
	for (const TSharedPtr<FStudioPolicyRule>& Rule : EditPolicy)
	{
		PolicyRows->AddSlot().AutoHeight().Padding(0.0f, 2.0f)[ MakePolicyRow(Rule) ];
	}
}

TSharedRef<SWidget> SCrowdyGameModelAdvancedTab::MakePolicyRow(TSharedPtr<FStudioPolicyRule> Rule)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// The contextual field lives in its own host so changing the rule type swaps just the field, without
	// destroying the type combo mid-callback. The field and Remove button edit the rule in place.
	const TSharedRef<SBox> FieldHost = SNew(SBox)[ MakePolicyRuleField(Rule) ];

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(170.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&PolicyTypeOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(PolicyTypeLabel(Item.IsValid() ? *Item : FString())); })
				.OnSelectionChanged_Lambda([this, Rule, FieldHost](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Rule->Type = *Item; FieldHost->SetContent(MakePolicyRuleField(Rule)); } })
				[ SNew(STextBlock).Text_Lambda([Rule]() { return PolicyTypeLabel(Rule->Type); }) ]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[ FieldHost ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(8.0f, 3.0f))
			.ToolTipText(LOCTEXT("PolRemoveTip", "Remove this requirement"))
			.OnClicked_Lambda([this, Rule]() { EditPolicy.Remove(Rule); RebuildPolicyRows(); return FReply::Handled(); })
			[ SNew(STextBlock).Text(LOCTEXT("PolRemove", "Remove")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
		];
}

TSharedRef<SWidget> SCrowdyGameModelAdvancedTab::MakePolicyRuleField(TSharedPtr<FStudioPolicyRule> Rule)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	if (Rule->Type == TEXT("tier_feature"))
	{
		return SNew(SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&PolicyFeatureOptions)
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
			.OnSelectionChanged_Lambda([Rule](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Rule->Feature = *Item; } })
			[ SNew(STextBlock).Text_Lambda([Rule]() { return FText::FromString(Rule->Feature.IsEmpty() ? FString(TEXT("feature...")) : Rule->Feature); }) ];
	}

	if (Rule->Type == TEXT("grid_permission"))
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&PolicyGridKeyOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
				.OnSelectionChanged_Lambda([Rule](TSharedPtr<FString> Item, ESelectInfo::Type) { if (Item.IsValid()) { Rule->Key = *Item; } })
				[ SNew(STextBlock).Text_Lambda([Rule]() { return FText::FromString(Rule->Key.IsEmpty() ? FString(TEXT("permission key...")) : Rule->Key); }) ]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(SBox).WidthOverride(120.0f)[ SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("PolGridId", "gridId (optional)")).Text(FText::FromString(Rule->GridId)).OnTextChanged_Lambda([Rule](const FText& T) { Rule->GridId = T.ToString(); }) ] ];
	}

	if (Rule->Type == TEXT("group_permission"))
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ SNew(SBox).WidthOverride(110.0f)[ SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("PolGroupId", "groupId")).Text(FText::FromString(Rule->GroupId)).OnTextChanged_Lambda([Rule](const FText& T) { Rule->GroupId = T.ToString(); }) ] ]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("PolPermission", "permission (optional)")).Text(FText::FromString(Rule->Permission)).OnTextChanged_Lambda([Rule](const FText& T) { Rule->Permission = T.ToString(); }) ];
	}

	if (Rule->Type == TEXT("condition"))
	{
		return SNew(SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("PolExpr", "expression, e.g. self.level >= 5"))
			.Text(FText::FromString(Rule->Expression))
			.OnTextChanged_Lambda([Rule](const FText& T) { Rule->Expression = T.ToString(); });
	}

	// owner_of_self / is_current_turn / is_host / is_participant carry no extra fields.
	return SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("PolNoFields", "no extra settings"));
}

TSharedRef<SWidget> SCrowdyGameModelAdvancedTab::MakeExpressionCheatSheet()
{
	auto Section = [](const FText& Title, const FString& Body) -> TSharedRef<SWidget>
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 2.0f)
			[ SNew(STextBlock).Text(Title).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			+ SVerticalBox::Slot().AutoHeight()
			[ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Body)).Font(FCoreStyle::GetDefaultFontStyle("Mono", 9)) ];
	};

	// Properties come from whatever container type is loaded in the list above (the same source the
	// mutation property dropdowns use); it may differ from the function's bound type until that type is
	// selected, so the label is honest about where they come from.
	FString PropsText;
	for (const TSharedPtr<FString>& Key : PropertyOptions)
	{
		if (Key.IsValid() && !Key->IsEmpty())
		{
			PropsText += (PropsText.IsEmpty() ? TEXT("") : TEXT("   ")) + *Key;
		}
	}
	if (PropsText.IsEmpty())
	{
		PropsText = TEXT("(select a type in the list above to see its properties)");
	}

	return SNew(SBox).WidthOverride(380.0f)
	[
		CrowdyStudioWidgets::Card(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprReads", "Reads"), TEXT("self.key    $param    ref($param).key    ref(\"<uuid>\").key")) ]
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprOps", "Operators"), TEXT("+  -  *  /  %      ==  !=  <  >  <=  >=      &&  ||  !")) ]
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprCond", "Conditional"), TEXT("if(condition, then, else)")) ]
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprBuiltins", "Builtins"), TEXT("max min abs floor ceil round clamp pow sqrt len concat to_int to_float to_string rand rand_int not is_null coalesce")) ]
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprCall", "Call another function (read-only)"), TEXT("fn:other_function(args)")) ]
			+ SVerticalBox::Slot().AutoHeight()[ Section(LOCTEXT("ExprProps", "Bound-type properties"), PropsText) ],
			FMargin(14.0f, 12.0f))
	];
}

void SCrowdyGameModelAdvancedTab::OnTypeSelectionChanged(TSharedPtr<FStudioContainerType> Type, ESelectInfo::Type)
{
	if (!Type.IsValid() || !Controller.IsValid())
	{
		return;
	}

	TypeNameBox->SetText(FText::FromString(Type->TypeName));
	DisplayNameBox->SetText(FText::FromString(Type->DisplayName));
	TypeDescBox->SetText(FText::FromString(Type->Description));
	TypeInstantiableBy = Type->InstantiableBy;
	TypeDefaultVis = Type->DefaultPropertyVisibility;

	Controller->FetchPropertyDefs(Type->TypeName);
	Controller->FetchFunctions(Type->TypeName);
}

void SCrowdyGameModelAdvancedTab::OnFunctionSelectionChanged(TSharedPtr<FStudioFunction> Function, ESelectInfo::Type)
{
	if (!Function.IsValid())
	{
		return;
	}

	FnNameBox->SetText(FText::FromString(Function->Name));
	FnTypeBox->SetText(FText::FromString(Function->ContainerTypeName));
	FnDescBox->SetText(FText::FromString(Function->Description));
	FnReturnTypeBox->SetText(FText::FromString(Function->ReturnType));
	FnInvokeScope = Function->InvokeScope;
	FnReturnExprBox->SetText(FText::FromString(Function->ReturnExpression));
	FnWarningsText = JoinWarnings(Function->Warnings);
	EditParams.Reset();
	for (const FStudioFunctionParam& Param : Function->Parameters)
	{
		EditParams.Add(MakeShared<FStudioFunctionParam>(Param));
	}
	RebuildParamsRows();
	EditMutations.Reset();
	for (const FStudioFunctionMutation& Mutation : Function->Mutations)
	{
		EditMutations.Add(MakeShared<FStudioFunctionMutation>(Mutation));
	}
	RebuildMutationRows();

	// Invoke policy: try the guided builder; if the stored tree is nested or unrecognized, fall back to
	// the raw JSON box (with a note) rather than lose it. The raw box always holds the original so the
	// escape hatch shows it verbatim.
	TArray<TSharedPtr<FStudioPolicyRule>> ParsedRules;
	FString ParsedConnector;
	const bool bRepresentable = CrowdyStudioFunctionJson::ParsePolicyJson(Function->InvokePolicyJson, ParsedRules, ParsedConnector);
	if (bRepresentable)
	{
		EditPolicy = ParsedRules;
		PolicyConnector = ParsedConnector;
	}
	else
	{
		EditPolicy.Reset();
	}
	bPolicyRawMode = !bRepresentable;
	bPolicyNotRepresentable = !bRepresentable;
	RebuildPolicyRows();
	if (FnPolicyBox.IsValid())
	{
		FnPolicyBox->SetText(FText::FromString(Function->InvokePolicyJson));
	}
}

void SCrowdyGameModelAdvancedTab::HandleAppChanged()
{
	SyncEditorAppScope();
}

void SCrowdyGameModelAdvancedTab::SyncEditorAppScope()
{
	const int64 CurrentAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;
	if (CurrentAppId == EditorAppId)
	{
		return;
	}

	EditorAppId = CurrentAppId;
	ClearAppScopedEditors();
}

void SCrowdyGameModelAdvancedTab::ClearAppScopedEditors()
{
	// The panes below are the only place an app switch cannot reach on its own: the lists come from the controller
	// and are already empty, but the editors keep their own copy of whatever was last selected. A name left in a box
	// here would let Save or Delete act on the new app while describing an entity that was only ever read from the
	// old one. There is no undo and no soft delete on the Game Model API, so clear all of it.
	auto ClearBox = [](const TSharedPtr<SEditableTextBox>& Box)
	{
		if (Box.IsValid())
		{
			Box->SetText(FText::GetEmpty());
		}
	};
	auto ClearMultiLineBox = [](const TSharedPtr<SMultiLineEditableTextBox>& Box)
	{
		if (Box.IsValid())
		{
			Box->SetText(FText::GetEmpty());
		}
	};

	// Container type editor.
	ClearBox(TypeNameBox);
	ClearBox(DisplayNameBox);
	ClearBox(TypeDescBox);
	TypeInstantiableBy = TEXT("member");
	TypeDefaultVis = TEXT("public");

	// Property editor. Its target type is read back out of TypeNameBox, which is now empty.
	ClearBox(PropKeyBox);
	ClearBox(PropDefaultBox);
	ClearBox(PropDescBox);
	PropValueType = TEXT("int");
	PropVis = TEXT("public");
	PropWritable = TEXT("function");

	// Function editor, including the parameter, mutation and invoke-policy rows.
	ClearBox(FnNameBox);
	ClearBox(FnTypeBox);
	ClearBox(FnDescBox);
	ClearBox(FnReturnTypeBox);
	ClearBox(FnReturnExprBox);
	ClearMultiLineBox(FnPolicyBox);
	FnInvokeScope = TEXT("player");
	FnWarningsText.Reset();
	EditParams.Reset();
	RebuildParamsRows();
	EditMutations.Reset();
	RebuildMutationRows();
	EditPolicy.Reset();
	PolicyConnector = TEXT("and");
	bPolicyRawMode = false;
	bPolicyNotRepresentable = false;
	RebuildPolicyRows();

	// Features and tier-feature grants. SelectedTier points at an entry of the access-tier list the controller has
	// just emptied, and a tier id means nothing outside the app it was listed for.
	ClearBox(FeatureKeyBox);
	ClearBox(FeatureDescBox);
	ClearBox(TierFeatureKeyBox);
	SelectedTier.Reset();
	if (TierComboBox.IsValid())
	{
		TierComboBox->ClearSelection();
	}

	// Session policy.
	ClearBox(ParticipantRoleBox);
	SessionCreationPolicy = TEXT("admin");

	// The bulk seed body was written against the previous app's schema, and Seed sends it verbatim to whichever app
	// is selected when the button is pressed.
	ClearMultiLineBox(SeedBox);

	// The rows still highlighted point into the lists the controller emptied on the switch.
	if (TypeListView.IsValid())
	{
		TypeListView->ClearSelection();
	}
	if (FunctionListView.IsValid())
	{
		FunctionListView->ClearSelection();
	}

	// The last deploy's outcome line named the previous app, and a deploy still running for it must not write its
	// outcome here once it lands.
	KitDeployStatus.Reset();
	KitDeployAppId = 0;
}

void SCrowdyGameModelAdvancedTab::HandleContainerTypesChanged()
{
	// An app switch empties this list before anything else happens, so this is the earliest and the most reliable
	// point at which the switch is visible to the tab.
	SyncEditorAppScope();

	if (TypeListView.IsValid())
	{
		TypeListView->RequestListRefresh();
	}

	// The mutation target dropdown lists the container types, so refresh it when they change.
	RebuildTargetOptions();
	RebuildMutationRows();
}

void SCrowdyGameModelAdvancedTab::HandlePropertyDefsChanged()
{
	if (PropertyListView.IsValid())
	{
		PropertyListView->RequestListRefresh();
	}

	// Offer the loaded container type's property keys as suggestions in the mutation property
	// dropdowns.
	PropertyOptions.Reset();
	if (Controller.IsValid())
	{
		for (const TSharedPtr<FStudioPropertyDef>& Def : Controller->GetPropertyDefs())
		{
			if (Def.IsValid())
			{
				PropertyOptions.Add(MakeShared<FString>(Def->Key));
			}
		}
	}
	RebuildMutationRows();
}

void SCrowdyGameModelAdvancedTab::HandleFunctionsChanged()
{
	if (FunctionListView.IsValid())
	{
		FunctionListView->RequestListRefresh();
	}

	// Keep the warnings panel in sync with the freshly fetched function (e.g. right after a save, whose
	// response carries the new static-analysis warnings). Match the function being edited by name.
	FnWarningsText.Reset();
	if (Controller.IsValid() && FnNameBox.IsValid())
	{
		const FString Name = FnNameBox->GetText().ToString();
		if (!Name.IsEmpty())
		{
			for (const TSharedPtr<FStudioFunction>& Fn : Controller->GetFunctions())
			{
				if (Fn.IsValid() && Fn->Name == Name)
				{
					FnWarningsText = JoinWarnings(Fn->Warnings);
					break;
				}
			}
		}
	}
}

void SCrowdyGameModelAdvancedTab::HandleFeaturesChanged()
{
	if (FeatureListView.IsValid())
	{
		FeatureListView->RequestListRefresh();
	}

	// Feed the tier_feature rule's dropdown in the policy builder.
	PolicyFeatureOptions.Reset();
	if (Controller.IsValid())
	{
		for (const TSharedPtr<FStudioAppFeature>& Feature : Controller->GetFeatures())
		{
			if (Feature.IsValid() && !Feature->FeatureKey.IsEmpty())
			{
				PolicyFeatureOptions.Add(MakeShared<FString>(Feature->FeatureKey));
			}
		}
	}
	RebuildPolicyRows();
}

void SCrowdyGameModelAdvancedTab::HandleTierFeaturesChanged()
{
	if (TierFeatureListView.IsValid())
	{
		TierFeatureListView->RequestListRefresh();
	}
}

void SCrowdyGameModelAdvancedTab::HandleAccessTiersChanged()
{
	if (TierComboBox.IsValid())
	{
		TierComboBox->RefreshOptions();
	}
}

void SCrowdyGameModelAdvancedTab::HandleRuntimePermissionsChanged()
{
	// Feed the grid_permission rule's key dropdown in the policy builder from the permission catalog.
	PolicyGridKeyOptions.Reset();
	if (Controller.IsValid())
	{
		for (const FString& Key : Controller->GetRuntimePermissions())
		{
			PolicyGridKeyOptions.Add(MakeShared<FString>(Key));
		}
	}
	RebuildPolicyRows();
}

FString SCrowdyGameModelAdvancedTab::SelectedTypeName() const
{
	if (TypeNameBox.IsValid())
	{
		return TypeNameBox->GetText().ToString();
	}
	return FString();
}

FText SCrowdyGameModelAdvancedTab::GetSelectedTypeLabel() const
{
	const FString TypeName = SelectedTypeName();
	if (TypeName.IsEmpty())
	{
		return LOCTEXT("PropsForType", "Properties (pick a container type above)");
	}
	return FText::Format(LOCTEXT("PropsOfFmt", "Properties of '{0}'"), FText::FromString(TypeName));
}

FText SCrowdyGameModelAdvancedTab::GetPolicyLabel() const
{
	if (Controller.IsValid())
	{
		const FStudioGameModelPolicy& Policy = Controller->GetGameModelPolicy();
		if (Policy.bValid)
		{
			return FText::Format(LOCTEXT("GmPolicyFmt", "Current: creation = {0},  default role = {1}"),
				FText::FromString(Policy.SessionCreationPolicy), FText::FromString(Policy.DefaultParticipantRole));
		}
	}
	return LOCTEXT("NoGmPolicy", "Current policy not loaded. Refresh to read it.");
}

TSharedRef<ITableRow> SCrowdyGameModelAdvancedTab::MakeTypeRow(TSharedPtr<FStudioContainerType> Type, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Display = Type.IsValid() ? Type->DisplayName : FString();
	const FString TypeName = Type.IsValid() ? Type->TypeName : FString();
	const FString By = Type.IsValid() ? Type->InstantiableBy : FString();

	return SNew(STableRow<TSharedPtr<FStudioContainerType>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ SNew(STextBlock).Text(FText::FromString(Display)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ CrowdyStudioWidgets::Chip(FText::FromString(TypeName)) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(FText::FromString(By)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyGameModelAdvancedTab::MakePropertyRow(TSharedPtr<FStudioPropertyDef> Def, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Key = Def.IsValid() ? Def->Key : FString();
	const FString Detail = Def.IsValid() ? FString::Printf(TEXT(": %s   ·   %s / %s"), *Def->ValueType, *Def->Visibility, *Def->Writable) : FString();

	return SNew(STableRow<TSharedPtr<FStudioPropertyDef>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ CrowdyStudioWidgets::Chip(FText::FromString(Key)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(Detail)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyGameModelAdvancedTab::MakeFunctionRow(TSharedPtr<FStudioFunction> Function, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Name = Function.IsValid() ? Function->Name : FString();
	const FString Bound = Function.IsValid() ? (Function->ContainerTypeName.IsEmpty() ? TEXT("global") : Function->ContainerTypeName) : FString();
	const FString Scope = Function.IsValid() ? Function->InvokeScope : FString();

	return SNew(STableRow<TSharedPtr<FStudioFunction>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ SNew(STextBlock).Text(FText::FromString(Name)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ CrowdyStudioWidgets::Chip(FText::FromString(Bound)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).HAlign(HAlign_Right)[ SNew(STextBlock).Text(FText::FromString(Scope)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyGameModelAdvancedTab::MakeFeatureRow(TSharedPtr<FStudioAppFeature> Feature, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Key = Feature.IsValid() ? Feature->FeatureKey : FString();
	const FString Desc = Feature.IsValid() ? Feature->Description : FString();

	return SNew(STableRow<TSharedPtr<FStudioAppFeature>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ CrowdyStudioWidgets::Chip(FText::FromString(Key)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(Desc)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyGameModelAdvancedTab::MakeTierFeatureRow(TSharedPtr<FStudioTierFeature> Grant, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Tier = Grant.IsValid() ? FString::Printf(TEXT("tier %lld"), Grant->TierId) : FString();
	const FString Key = Grant.IsValid() ? Grant->FeatureKey : FString();

	return SNew(STableRow<TSharedPtr<FStudioTierFeature>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ CrowdyStudioWidgets::Chip(FText::FromString(Tier)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)[ SNew(STextBlock).Text(FText::FromString(Key)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
			]
		];
}

#undef LOCTEXT_NAMESPACE
