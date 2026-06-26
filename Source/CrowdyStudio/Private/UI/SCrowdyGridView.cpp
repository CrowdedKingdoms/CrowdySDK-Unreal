// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyGridView.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#include "Editor.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/MessageDialog.h"
#include "Subsystem/CrowdyGameSession.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyGridView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnNearbyGridsChanged.AddSP(this, &SCrowdyGridView::HandleGridsChanged);
		Controller->OnGridDetailChanged.AddSP(this, &SCrowdyGridView::HandleDetailChanged);
		Controller->OnGridWhitelistChanged.AddSP(this, &SCrowdyGridView::HandleWhitelistChanged);
		Controller->OnRuntimePermissionsChanged.AddSP(this, &SCrowdyGridView::HandlePermissionCatalogChanged);
		Controller->OnAccessTiersChanged.AddSP(this, &SCrowdyGridView::HandleTiersChanged);
	}

	// Follow the active world: PIE start/stop changes which world the boxes must be drawn into.
	PieStartHandle = FEditorDelegates::PostPIEStarted.AddSP(this, &SCrowdyGridView::HandlePieEvent);
	PieEndHandle = FEditorDelegates::EndPIE.AddSP(this, &SCrowdyGridView::HandlePieEvent);

	// Live "from selection" preview: redraw when the editor selection changes.
	SelectionChangedHandle = USelection::SelectionChangedEvent.AddSP(this, &SCrowdyGridView::HandleSelectionChanged);

	// Selection-changed fires when picking actors, but not while one is dragged; poll at a low rate to
	// follow a drag into another chunk (cheap - MaybeRedrawForSelection only redraws on a real change).
	SelectionPollHandle = RegisterActiveTimer(0.1f, FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyGridView::PollSelectionForMove));

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	auto AxisBox = [&Style](TSharedPtr<SEditableTextBox>& Member, const TCHAR* Hint) -> TSharedRef<SWidget>
	{
		return SAssignNew(Member, SEditableTextBox).Style(&Style, "Crowdy.Input")
			.HintText(FText::FromString(Hint)).MinDesiredWidth(44.0f).SelectAllTextWhenFocused(true);
	};

	// Like AxisBox, but live-updates the viewport preview as a grid corner is typed (B2 live preview).
	auto CornerBox = [&Style, this](TSharedPtr<SEditableTextBox>& Member, const TCHAR* Hint) -> TSharedRef<SWidget>
	{
		return SAssignNew(Member, SEditableTextBox).Style(&Style, "Crowdy.Input")
			.HintText(FText::FromString(Hint)).MinDesiredWidth(44.0f).SelectAllTextWhenFocused(true)
			.OnTextChanged_Lambda([this](const FText&) { RefreshGridVisualization(); });
	};

	auto Btn = [&Style](const FText& Label, bool bPrimary, FOnClicked OnClick) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.ButtonStyle(&Style, bPrimary ? "Crowdy.Button.Primary" : "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(12.0f, 6.0f))
			.OnClicked(OnClick)
			[ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ];
	};

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(2.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[ SNew(STextBlock).Text(LOCTEXT("GridHeader", "Grid")).TextStyle(&Style, "Crowdy.Text.Title") ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("GridPlane", "World regions that voxel/runtime permissions scope to. Game plane — sign in with email and password.")) ]

			// Create grid.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("CreateGrid", "Create grid"), TEXT("plus")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[ SNew(STextBlock).Text(LOCTEXT("Corner1", "Corner 1")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)[ CornerBox(Corner1X, TEXT("x")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)[ CornerBox(Corner1Y, TEXT("y")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)[ CornerBox(Corner1Z, TEXT("z")) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 6.0f, 0.0f)
						[ SNew(STextBlock).Text(LOCTEXT("Corner2", "Corner 2")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)[ CornerBox(Corner2X, TEXT("x")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)[ CornerBox(Corner2Y, TEXT("y")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)[ CornerBox(Corner2Z, TEXT("z")) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[ Btn(LOCTEXT("ResetCornersButton", "Reset"), false, FOnClicked::CreateSP(this, &SCrowdyGridView::OnResetCornersClicked)) ]
							+ SHorizontalBox::Slot().AutoWidth()
							[ Btn(LOCTEXT("CreateGridButton", "Create"), true, FOnClicked::CreateSP(this, &SCrowdyGridView::OnCreateGridClicked)) ]
						]
					],
					FMargin(16.0f, 14.0f))
			]

			// Create grid from selection (B2-1): derive the chunk corners from selected level actors.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("CreateFromSelection", "Create grid from selection"), TEXT("plus")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("CreateFromSelectionHint", "Select one or more actors in the level, then create a grid from the chunks they occupy. With no running session the editor uses the preview grid size set below (Visualize in viewport), which may differ from a session's configured size.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([this]() { return bSelectionByLocation ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { bSelectionByLocation = (NewState == ECheckBoxState::Checked); MaybeRedrawForSelection(); })
							[ SNew(STextBlock).Margin(FMargin(4.0f, 0.0f, 0.0f, 0.0f)).Text(LOCTEXT("SelByLocation", "Map by actor location (one chunk each)")).TextStyle(&Style, "Crowdy.Text.Body") ]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
						[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("SelByLocationHint", "On: each actor maps to the single chunk it sits in (matches the runtime). Off: cover every chunk the actor's collision bounds touch.")) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
						[ Btn(LOCTEXT("CreateFromSelectionButton", "Create grid from selection"), true, FOnClicked::CreateSP(this, &SCrowdyGridView::OnCreateFromSelectionClicked)) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(this, &SCrowdyGridView::GetSelectionNote) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Scan for grids.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("ScanRegion", "Scan for grids"), TEXT("inspector")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("ScanHint", "Grids have no list-all query; scan a chunk region as a user to discover them.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[ SAssignNew(ScanUserBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ScanUserHint", "userId")).MinDesiredWidth(78.0f).SelectAllTextWhenFocused(true) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 2.0f, 0.0f)[ AxisBox(ScanLowX, TEXT("lowX")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)[ AxisBox(ScanLowY, TEXT("lowY")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)[ AxisBox(ScanLowZ, TEXT("lowZ")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 2.0f, 0.0f)[ AxisBox(ScanHighX, TEXT("highX")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)[ AxisBox(ScanHighY, TEXT("highY")) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f)[ AxisBox(ScanHighZ, TEXT("highZ")) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Right).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[ Btn(LOCTEXT("ScanButton", "Scan"), false, FOnClicked::CreateSP(this, &SCrowdyGridView::OnScanClicked)) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox).HeightOverride(150.0f)
						[
							CrowdyStudioWidgets::Card(
								SNew(SOverlay)
								+ SOverlay::Slot()
								[
									SAssignNew(GridListView, SListView<TSharedPtr<FStudioGrid>>)
									.ListItemsSource(Controller.IsValid() ? &Controller->GetNearbyGrids() : nullptr)
									.OnGenerateRow(this, &SCrowdyGridView::MakeGridRow)
									.OnSelectionChanged(this, &SCrowdyGridView::HandleGridSelectionChanged)
									.SelectionMode(ESelectionMode::Single)
								]
								+ SOverlay::Slot()
								[
									SNew(SBox).Visibility_Lambda([this]() { return (Controller.IsValid() && Controller->GetNearbyGrids().Num() == 0) ? EVisibility::Visible : EVisibility::Collapsed; })
									[ CrowdyStudioWidgets::EmptyState(TEXT("grid"), LOCTEXT("NoGrids", "No grids found.\nScan a region to discover them.")) ]
								],
								FMargin(4.0f), /*bFlat*/ true)
						]
					],
					FMargin(16.0f, 14.0f))
			]

			// Visualize grids in the viewport (B2-2 + live preview): editor and PIE.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("VisualizeGrids", "Visualize in viewport"), TEXT("grid")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([this]() { return bShowGridViz ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { SetShowGridViz(NewState == ECheckBoxState::Checked); })
							[ SNew(STextBlock).Margin(FMargin(4.0f, 0.0f, 0.0f, 0.0f)).Text(LOCTEXT("DrawGrids", "Show grids in viewport")).TextStyle(&Style, "Crowdy.Text.Body") ]
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
						[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("DrawGridsHint", "Draws debug boxes in the level (editor and Play-In-Editor): red is the grid you are about to create (from the corners above), cyan are existing grids from the last scan.")) ]
					]

					// Edit-time preview scale: the real chunk size comes from the game session at runtime;
					// this only scales the debug boxes for authoring (see ResolveChunkSize). Disabled in PIE.
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[ SNew(STextBlock).Text(LOCTEXT("GridSizeLabel", "Preview grid size")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SBox).WidthOverride(110.0f)
							[
								SNew(SSpinBox<double>)
								.MinValue(1.0).Delta(10.0).LinearDeltaSensitivity(1)
								.Value_Lambda([this]() { bool bLive = false; return ResolveChunkSize(bLive); })
								.IsEnabled_Lambda([this]() { return FindRunningGameWorld() == nullptr; })
								.OnValueChanged_Lambda([this](double NewValue) { EditTimeChunkSize = NewValue; RefreshGridVisualization(); })
							]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("GridSizeHint", "Visual authoring only - scales the preview boxes in the editor. The real chunk size is set on the game session at runtime (UCrowdyGameSession::SetChunkSize). While Play-In-Editor is running, the live session's size is used instead.")) ]
					,
					FMargin(16.0f, 14.0f))
			]

			// Effective-permissions simulator (what-if): preview a user's keys on the selected grid.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("SimHeader", "Effective permissions (what-if)"), TEXT("login")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("SimHint", "Preview which permission keys a user would have on the selected grid, and where each comes from. Pick a tier to simulate; the grid grants and whitelist are read from the server.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							SNew(SBox).WidthOverride(180.0f)
							[
								SAssignNew(SimTierCombo, SComboBox<TSharedPtr<FStudioAccessTier>>)
								.OptionsSource(Controller.IsValid() ? &Controller->GetAccessTiers() : nullptr)
								.OnGenerateWidget_Lambda([](TSharedPtr<FStudioAccessTier> Tier) { return SNew(STextBlock).Text(FText::FromString(Tier.IsValid() ? FString::Printf(TEXT("%s  (#%lld)"), *Tier->Name, Tier->TierId) : FString())); })
								.OnSelectionChanged_Lambda([this](TSharedPtr<FStudioAccessTier> Tier, ESelectInfo::Type) { SimSelectedTier = Tier; RebuildSimulator(); })
								[ SNew(STextBlock).Text_Lambda([this]() { return SimSelectedTier.IsValid() ? FText::FromString(SimSelectedTier->Name) : LOCTEXT("SimPickTier", "No tier (grants only)"); }) ]
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[ SAssignNew(SimUserBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("SimUserHint", "userId")).MinDesiredWidth(90.0f).SelectAllTextWhenFocused(true) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ Btn(LOCTEXT("SimulateButton", "Simulate"), true, FOnClicked::CreateSP(this, &SCrowdyGridView::OnSimulateClicked)) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[ SAssignNew(SimResultHost, SBox) ],
					FMargin(16.0f, 14.0f))
			]

			// Selected grid detail.
			+ SVerticalBox::Slot().AutoHeight()
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("GridDetail", "Selected grid"), TEXT("grid")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(this, &SCrowdyGridView::GetSelectedGridLabel) ]

					// Whitelist (limits): caps which keys can ever take effect on the grid, regardless of grants.
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.BodyStrong").Text(this, &SCrowdyGridView::GetWhitelistLabel) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ SAssignNew(WhitelistPickerHost, SBox)[ BuildPermissionPicker(&WhitelistSelection) ] ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("WhitelistPickerHint", "None checked = no limit (every key allowed).")) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ Btn(LOCTEXT("SetWhitelist", "Set Whitelist"), false, FOnClicked::CreateSP(this, &SCrowdyGridView::OnSetWhitelistClicked)) ]
					]

					// User grants.
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("UserGrants", "User grants"), TEXT("login")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ SAssignNew(UserIdBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("UserIdHint", "userId")).MinDesiredWidth(78.0f).SelectAllTextWhenFocused(true) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ SAssignNew(UserKeysPickerHost, SBox)[ BuildPermissionPicker(&UserKeysSelection) ] ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)[ Btn(LOCTEXT("GrantUser", "Grant"), false, FOnClicked::CreateSP(this, &SCrowdyGridView::OnGrantUserClicked)) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)[ Btn(LOCTEXT("RevokeUser", "Revoke"), false, FOnClicked::CreateSP(this, &SCrowdyGridView::OnRevokeUserClicked)) ]
						+ SHorizontalBox::Slot().AutoWidth()[ Btn(LOCTEXT("ReadUser", "Read"), false, FOnClicked::CreateSP(this, &SCrowdyGridView::OnReadUserClicked)) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 14.0f)
					[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(this, &SCrowdyGridView::GetUserEffectiveLabel) ]

					// Group grants.
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("GroupGrants", "Group grants"), TEXT("users")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[ SAssignNew(GroupIdBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("GroupIdHint", "groupId")).MinDesiredWidth(78.0f).SelectAllTextWhenFocused(true) ]
						+ SHorizontalBox::Slot().AutoWidth()
						[ SAssignNew(GroupRoleBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("GroupRoleHint", "roleId (optional)")).MinDesiredWidth(96.0f).SelectAllTextWhenFocused(true) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ SAssignNew(GroupKeysPickerHost, SBox)[ BuildPermissionPicker(&GroupKeysSelection) ] ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)[ Btn(LOCTEXT("AssignGroup", "Assign"), false, FOnClicked::CreateSP(this, &SCrowdyGridView::OnAssignGroupClicked)) ]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f)[ Btn(LOCTEXT("RevokeGroup", "Revoke"), false, FOnClicked::CreateSP(this, &SCrowdyGridView::OnRevokeGroupClicked)) ]
						+ SHorizontalBox::Slot().AutoWidth()[ Btn(LOCTEXT("ListGroupGrants", "List Grants"), false, FOnClicked::CreateSP(this, &SCrowdyGridView::OnListGroupGrantsClicked)) ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBox).HeightOverride(120.0f)
						[
							CrowdyStudioWidgets::Card(
								SAssignNew(GrantListView, SListView<TSharedPtr<FStudioGridGroupGrant>>)
								.ListItemsSource(Controller.IsValid() ? &Controller->GetGridGroupGrants() : nullptr)
								.OnGenerateRow(this, &SCrowdyGridView::MakeGrantRow)
								.SelectionMode(ESelectionMode::None),
								FMargin(4.0f), /*bFlat*/ true)
						]
					],
					FMargin(16.0f, 14.0f))
			]
		]
	];

	// Seed the simulator's placeholder (no grid selected yet).
	RebuildSimulator();
}

SCrowdyGridView::~SCrowdyGridView()
{
	if (Controller.IsValid())
	{
		Controller->OnNearbyGridsChanged.RemoveAll(this);
		Controller->OnGridDetailChanged.RemoveAll(this);
		Controller->OnGridWhitelistChanged.RemoveAll(this);
		Controller->OnRuntimePermissionsChanged.RemoveAll(this);
		Controller->OnAccessTiersChanged.RemoveAll(this);
	}

	FEditorDelegates::PostPIEStarted.Remove(PieStartHandle);
	FEditorDelegates::EndPIE.Remove(PieEndHandle);
	USelection::SelectionChangedEvent.Remove(SelectionChangedHandle);
	if (SelectionPollHandle.IsValid())
	{
		UnRegisterActiveTimer(SelectionPollHandle.ToSharedRef());
	}
	// Don't leave grid boxes behind in the level when the console closes.
	ClearGridVisualization();
}

int64 SCrowdyGridView::ParseInt(const TSharedPtr<SEditableTextBox>& Box)
{
	return Box.IsValid() ? FCString::Atoi64(*Box->GetText().ToString()) : 0;
}

TArray<FString> SCrowdyGridView::SelectionToKeys(const TArray<FString>& CatalogOrder, const TSet<FString>& Selection)
{
	// Emit selected keys in catalog order (the server's bit-index order) so the payload is stable.
	TArray<FString> Keys;
	for (const FString& Key : CatalogOrder)
	{
		if (Selection.Contains(Key))
		{
			Keys.Add(Key);
		}
	}
	return Keys;
}

TSharedRef<SWidget> SCrowdyGridView::BuildPermissionPicker(TSet<FString>* Selection)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	if (!Controller.IsValid() || Controller->GetRuntimePermissions().Num() == 0)
	{
		return SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle")
			.Text(LOCTEXT("NoCatalog", "Scan to load the permission catalog."));
	}

	const TSharedRef<SWrapBox> Wrap = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(14.0f, 6.0f));
	for (const FString& Key : Controller->GetRuntimePermissions())
	{
		// Selection points at a member set that outlives this widget tree, and Key is copied, so the
		// bound lambdas stay valid for the picker's lifetime.
		Wrap->AddSlot()
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([Selection, Key]() { return Selection->Contains(Key) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Selection, Key](ECheckBoxState NewState)
			{
				if (NewState == ECheckBoxState::Checked) { Selection->Add(Key); }
				else { Selection->Remove(Key); }
			})
			[
				SNew(STextBlock).Margin(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
				.Text(FText::FromString(Key))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
			]
		];
	}
	return Wrap;
}

void SCrowdyGridView::RebuildPermissionPickers()
{
	if (WhitelistPickerHost.IsValid()) { WhitelistPickerHost->SetContent(BuildPermissionPicker(&WhitelistSelection)); }
	if (UserKeysPickerHost.IsValid()) { UserKeysPickerHost->SetContent(BuildPermissionPicker(&UserKeysSelection)); }
	if (GroupKeysPickerHost.IsValid()) { GroupKeysPickerHost->SetContent(BuildPermissionPicker(&GroupKeysSelection)); }
}

int64 SCrowdyGridView::SelectedGridId() const
{
	if (GridListView.IsValid())
	{
		const TArray<TSharedPtr<FStudioGrid>> Selected = GridListView->GetSelectedItems();
		if (Selected.Num() > 0 && Selected[0].IsValid())
		{
			return Selected[0]->GridId;
		}
	}
	return 0;
}

FReply SCrowdyGridView::OnCreateGridClicked()
{
	if (Controller.IsValid())
	{
		Controller->CreateGrid(
			ParseInt(Corner1X), ParseInt(Corner1Y), ParseInt(Corner1Z),
			ParseInt(Corner2X), ParseInt(Corner2Y), ParseInt(Corner2Z));
	}
	return FReply::Handled();
}

FReply SCrowdyGridView::OnResetCornersClicked()
{
	// Clear all six corner fields in one click instead of erasing each by hand.
	auto Clear = [](const TSharedPtr<SEditableTextBox>& Box)
	{
		if (Box.IsValid())
		{
			Box->SetText(FText::GetEmpty());
		}
	};
	Clear(Corner1X); Clear(Corner1Y); Clear(Corner1Z);
	Clear(Corner2X); Clear(Corner2Y); Clear(Corner2Z);
	// SetText doesn't fire OnTextChanged, so refresh to drop the now-empty corner preview box.
	RefreshGridVisualization();
	return FReply::Handled();
}

FReply SCrowdyGridView::OnScanClicked()
{
	if (Controller.IsValid())
	{
		// Load the permission catalog alongside the scan so the detail pickers are populated by the
		// time a grid is selected. The catalog is public, so this works regardless of the game token.
		Controller->FetchRuntimePermissions();
		// Load the access tiers too, so the simulator's tier dropdown is ready.
		Controller->FetchAppAccessTiers();
		Controller->FetchNearbyGrids(ParseInt(ScanUserBox),
			ParseInt(ScanLowX), ParseInt(ScanLowY), ParseInt(ScanLowZ),
			ParseInt(ScanHighX), ParseInt(ScanHighY), ParseInt(ScanHighZ));
	}
	return FReply::Handled();
}

FReply SCrowdyGridView::OnSetWhitelistClicked()
{
	if (Controller.IsValid())
	{
		Controller->SetGridPermissionLimits(SelectedGridId(), SelectionToKeys(Controller->GetRuntimePermissions(), WhitelistSelection));
	}
	return FReply::Handled();
}

FReply SCrowdyGridView::OnGrantUserClicked()
{
	if (Controller.IsValid())
	{
		Controller->GrantGridPermissions(SelectedGridId(), ParseInt(UserIdBox), SelectionToKeys(Controller->GetRuntimePermissions(), UserKeysSelection));
	}
	return FReply::Handled();
}

FReply SCrowdyGridView::OnRevokeUserClicked()
{
	if (Controller.IsValid())
	{
		Controller->RevokeGridPermissions(SelectedGridId(), ParseInt(UserIdBox), SelectionToKeys(Controller->GetRuntimePermissions(), UserKeysSelection));
	}
	return FReply::Handled();
}

FReply SCrowdyGridView::OnReadUserClicked()
{
	if (Controller.IsValid())
	{
		Controller->FetchGridUserPermissions(SelectedGridId(), ParseInt(UserIdBox));
	}
	return FReply::Handled();
}

FReply SCrowdyGridView::OnAssignGroupClicked()
{
	if (Controller.IsValid())
	{
		const int64 RoleId = ParseInt(GroupRoleBox);
		Controller->AssignGroupToGrid(SelectedGridId(), ParseInt(GroupIdBox), RoleId != 0, RoleId, SelectionToKeys(Controller->GetRuntimePermissions(), GroupKeysSelection));
	}
	return FReply::Handled();
}

FReply SCrowdyGridView::OnRevokeGroupClicked()
{
	if (Controller.IsValid())
	{
		const int64 RoleId = ParseInt(GroupRoleBox);
		Controller->RevokeGroupFromGrid(SelectedGridId(), ParseInt(GroupIdBox), RoleId != 0, RoleId, SelectionToKeys(Controller->GetRuntimePermissions(), GroupKeysSelection));
	}
	return FReply::Handled();
}

FReply SCrowdyGridView::OnListGroupGrantsClicked()
{
	if (Controller.IsValid())
	{
		Controller->FetchGridGroupGrants(SelectedGridId(), ParseInt(GroupIdBox));
	}
	return FReply::Handled();
}

void SCrowdyGridView::HandleGridsChanged()
{
	if (GridListView.IsValid())
	{
		GridListView->RequestListRefresh();
	}
	// A scan just changed the set of existing grids; redraw the viewport boxes to match.
	RefreshGridVisualization();
}

void SCrowdyGridView::HandleDetailChanged()
{
	if (GrantListView.IsValid())
	{
		GrantListView->RequestListRefresh();
	}
	// A user's effective keys may have just loaded (FetchGridUserPermissions fires this), so refresh
	// the what-if breakdown.
	RebuildSimulator();
}

void SCrowdyGridView::HandleWhitelistChanged()
{
	// Resync the whitelist checkboxes to the server's current limits for the selected grid. The
	// checkbox state binds to WhitelistSelection live, so mutating the set is enough to refresh the UI.
	WhitelistSelection.Reset();
	if (Controller.IsValid())
	{
		for (const FString& Key : Controller->GetGridWhitelistKeys())
		{
			WhitelistSelection.Add(Key);
		}
	}
	// The whitelist gates what can take effect, so re-run the what-if breakdown.
	RebuildSimulator();
}

void SCrowdyGridView::HandlePermissionCatalogChanged()
{
	RebuildPermissionPickers();
}

void SCrowdyGridView::HandleGridSelectionChanged(TSharedPtr<FStudioGrid> Grid, ESelectInfo::Type /*SelectInfo*/)
{
	// Load the newly selected grid's whitelist so its checkboxes reflect the live limits. Clear first
	// so the previous grid's whitelist does not linger while the fetch is in flight. User/group key
	// selections are left intact (they are grant intent, not server state).
	WhitelistSelection.Reset();
	if (Controller.IsValid() && Grid.IsValid() && Grid->GridId != 0)
	{
		Controller->FetchGridPermissionLimits(Grid->GridId);
	}
	// Reflect the new grid selection (the simulator keys off the selected grid).
	RebuildSimulator();
}

FText SCrowdyGridView::GetSelectedGridLabel() const
{
	const int64 GridId = SelectedGridId();
	if (GridId == 0)
	{
		return LOCTEXT("NoGridSelected", "Scan a region, then pick a grid to edit its permissions.");
	}
	return FText::Format(LOCTEXT("EditingGridFmt", "Editing grid #{0}"), FText::AsNumber(GridId));
}

FText SCrowdyGridView::GetWhitelistLabel() const
{
	if (Controller.IsValid())
	{
		const TArray<FString>& Keys = Controller->GetGridWhitelistKeys();
		const FString Joined = Keys.Num() > 0 ? FString::Join(Keys, TEXT(", ")) : TEXT("(no limit)");
		return FText::FromString(FString::Printf(TEXT("Permission whitelist: %s"), *Joined));
	}
	return LOCTEXT("WhitelistLabel", "Permission whitelist");
}

FText SCrowdyGridView::GetUserEffectiveLabel() const
{
	if (Controller.IsValid())
	{
		const TArray<FString>& Keys = Controller->GetGridUserEffectiveKeys();
		if (Keys.Num() > 0)
		{
			return FText::FromString(FString::Printf(TEXT("Effective: %s"), *FString::Join(Keys, TEXT(", "))));
		}
	}
	return LOCTEXT("NoEffective", "Effective: read a user to see their keys on this grid.");
}

TSharedRef<ITableRow> SCrowdyGridView::MakeGridRow(TSharedPtr<FStudioGrid> Grid, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Id = Grid.IsValid() ? FString::Printf(TEXT("#%lld"), Grid->GridId) : FString();
	const FString Coords = Grid.IsValid()
		? FString::Printf(TEXT("[%lld, %lld, %lld]  →  [%lld, %lld, %lld]"), Grid->Low.X, Grid->Low.Y, Grid->Low.Z, Grid->High.X, Grid->High.Y, Grid->High.Z)
		: FString();
	const int32 KeyCount = Grid.IsValid() ? Grid->EffectivePermissionKeys.Num() : 0;

	return SNew(STableRow<TSharedPtr<FStudioGrid>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 8.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ CrowdyStudioWidgets::Chip(FText::FromString(Id)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(FText::FromString(Coords)).Font(FCoreStyle::GetDefaultFontStyle("Mono", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ CrowdyStudioWidgets::Badge(FText::Format(LOCTEXT("KeyCountFmt", "{0} keys"), FText::AsNumber(KeyCount)), CrowdyStudioWidgets::EBadgeTone::Info) ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyGridView::MakeGrantRow(TSharedPtr<FStudioGridGroupGrant> Grant, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Key = Grant.IsValid() ? Grant->PermissionKey : FString();
	const FString Detail = Grant.IsValid()
		? FString::Printf(TEXT("group %lld · %s%s"), Grant->GroupId,
			Grant->bHasRole ? *FString::Printf(TEXT("role %lld"), Grant->GroupRoleId) : TEXT("all roles"),
			Grant->ExpiresAt.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" · expires %s"), *Grant->ExpiresAt))
		: FString();

	return SNew(STableRow<TSharedPtr<FStudioGridGroupGrant>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ CrowdyStudioWidgets::Chip(FText::FromString(Key)) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(FText::FromString(Detail)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
			]
		];
}

void SCrowdyGridView::HandleTiersChanged()
{
	if (SimTierCombo.IsValid())
	{
		SimTierCombo->RefreshOptions();
	}
	RebuildSimulator();
}

FReply SCrowdyGridView::OnSimulateClicked()
{
	if (Controller.IsValid())
	{
		const int64 GridId = SelectedGridId();
		if (GridId == 0)
		{
			RebuildSimulator(); // shows the "select a grid" hint
			return FReply::Handled();
		}
		// Refresh the inputs the breakdown composes: the access tiers, the grid whitelist, and the
		// sim user's effective keys on this grid. Each fetch fires a delegate that rebuilds the breakdown.
		Controller->FetchAppAccessTiers();
		Controller->FetchGridPermissionLimits(GridId);
		Controller->FetchGridUserPermissions(GridId, ParseInt(SimUserBox));
	}
	return FReply::Handled();
}

void SCrowdyGridView::RebuildSimulator()
{
	if (!SimResultHost.IsValid())
	{
		return;
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	const int64 GridId = SelectedGridId();
	if (GridId == 0)
	{
		SimResultHost->SetContent(
			SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle")
			.Text(LOCTEXT("SimNoGrid", "Select a grid above, then pick a tier and a user and press Simulate.")));
		return;
	}

	if (!Controller.IsValid() || Controller->GetRuntimePermissions().Num() == 0)
	{
		SimResultHost->SetContent(
			SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle")
			.Text(LOCTEXT("SimNoCatalog", "Scan to load the permission catalog first.")));
		return;
	}

	// effective = (tier keys union the user's grid grants) intersect the grid whitelist; admins bypass
	// all of this server-side. We compose the same inputs here for a transparent, per-key preview.
	const TArray<FString>& Catalog = Controller->GetRuntimePermissions();
	const TSet<FString> TierKeys = SimSelectedTier.IsValid() ? TSet<FString>(SimSelectedTier->PermissionKeys) : TSet<FString>();
	const TSet<FString> UserKeys(Controller->GetGridUserEffectiveKeys());
	const TArray<FString>& Whitelist = Controller->GetGridWhitelistKeys();
	const TSet<FString> WhiteSet(Whitelist);
	const bool bHasWhitelist = Whitelist.Num() > 0;
	const FString TierName = SimSelectedTier.IsValid() ? SimSelectedTier->Name : FString();

	TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
	TArray<FString> Effective;
	int32 Shown = 0;

	for (const FString& Key : Catalog)
	{
		const bool bInTier = TierKeys.Contains(Key);
		const bool bInUser = UserKeys.Contains(Key);
		if (!bInTier && !bInUser)
		{
			continue; // not granted by any source; omit to keep the breakdown focused
		}
		const bool bWhitelisted = !bHasWhitelist || WhiteSet.Contains(Key);

		TArray<FString> Sources;
		if (bInTier) { Sources.Add(TierName.IsEmpty() ? FString(TEXT("tier")) : FString::Printf(TEXT("tier %s"), *TierName)); }
		if (bInUser) { Sources.Add(TEXT("grid grant")); }
		const FString SourceText = FString::Join(Sources, TEXT(" + "));

		FString StatusText;
		FLinearColor Color;
		if (bWhitelisted)
		{
			Effective.Add(Key);
			StatusText = FString::Printf(TEXT("allowed (from %s)"), *SourceText);
			Color = FCrowdyStudioStyle::Success();
		}
		else
		{
			StatusText = FString::Printf(TEXT("blocked by grid whitelist (would come from %s)"), *SourceText);
			Color = FCrowdyStudioStyle::Danger();
		}

		List->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[ CrowdyStudioWidgets::Chip(FText::FromString(Key)) ]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(StatusText)).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(Color)) ]
		];
		++Shown;
	}

	const FString Summary = Effective.Num() > 0
		? FString::Printf(TEXT("Effective: %s"), *FString::Join(Effective, TEXT(", ")))
		: FString(TEXT("Effective: none with the current tier and grid grants."));

	SimResultHost->SetContent(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.BodyStrong").Text(FText::FromString(Summary)) ]
		+ SVerticalBox::Slot().AutoHeight()
		[
			Shown > 0
				? StaticCastSharedRef<SWidget>(List)
				: StaticCastSharedRef<SWidget>(SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("SimNothing", "No keys from the selected tier; press Simulate after entering a user to include their grid grants.")))
		]);
}

// ---- B2: visual grid authoring -----------------------------------------------------------------

UWorld* SCrowdyGridView::FindRunningGameWorld()
{
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType != EWorldType::PIE && Context.WorldType != EWorldType::Game)
		{
			continue;
		}
		if (UWorld* World = Context.World())
		{
			return World;
		}
	}
	return nullptr;
}

double SCrowdyGridView::ResolveChunkSize(bool& bOutFromLiveSession) const
{
	bOutFromLiveSession = false;
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType != EWorldType::PIE && Context.WorldType != EWorldType::Game)
			{
				continue;
			}
			if (const UGameInstance* GameInstance = Context.OwningGameInstance)
			{
				if (const UCrowdyGameSession* Session = GameInstance->GetSubsystem<UCrowdyGameSession>())
				{
					bOutFromLiveSession = true;
					return Session->GetChunkSize();
				}
			}
		}
	}
	// No running session: fall back to the edit-time preview size so editor-time authoring still works.
	return EditTimeChunkSize;
}

FText SCrowdyGridView::GetSelectionNote() const
{
	return SelectionNote.IsEmpty()
		? LOCTEXT("SelectionNoteIdle", "Select actors in the level first, then create a grid from their combined bounds.")
		: SelectionNote;
}

FReply SCrowdyGridView::OnCreateFromSelectionClicked()
{
	if (!Controller.IsValid())
	{
		return FReply::Handled();
	}
	if (!GEditor)
	{
		SelectionNote = LOCTEXT("NoEditor", "The editor selection is unavailable.");
		return FReply::Handled();
	}

	TArray<AActor*> Selected;
	GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Selected);
	if (Selected.Num() == 0)
	{
		SelectionNote = LOCTEXT("NoSelection", "No actors selected. Pick one or more actors in the level, then try again.");
		return FReply::Handled();
	}

	bool bLiveChunkSize = false;
	const double ChunkSize = ResolveChunkSize(bLiveChunkSize);

	FStudioChunk Low, High;
	if (!TryGetSelectionChunks(Low, High, ChunkSize))
	{
		SelectionNote = LOCTEXT("NoBounds", "The selected actors have no bounds to derive a grid from.");
		return FReply::Handled();
	}
	const int64 LoX = Low.X, LoY = Low.Y, LoZ = Low.Z;
	const int64 HiX = High.X, HiY = High.Y, HiZ = High.Z;

	const FText Prompt = FText::FromString(FString::Printf(
		TEXT("Create a grid spanning chunks [%lld, %lld, %lld] to [%lld, %lld, %lld]?\n\nDerived from %d selected actor(s) using chunk size %.0f (%s)."),
		LoX, LoY, LoZ, HiX, HiY, HiZ, Selected.Num(), ChunkSize,
		bLiveChunkSize ? TEXT("from the running session") : TEXT("editor preview size; start Play-In-Editor to use the session's size")));

	if (FMessageDialog::Open(EAppMsgType::OkCancel, Prompt) != EAppReturnType::Ok)
	{
		SelectionNote = LOCTEXT("SelectionCancelled", "Cancelled.");
		return FReply::Handled();
	}

	// Create from the selection without touching the corner fields - the two authoring paths are
	// independent. The orange "grid from selection" preview already shows what this creates.
	Controller->CreateGrid(LoX, LoY, LoZ, HiX, HiY, HiZ);
	SelectionNote = FText::FromString(FString::Printf(
		TEXT("Creating grid for chunks [%lld, %lld, %lld] to [%lld, %lld, %lld]."),
		LoX, LoY, LoZ, HiX, HiY, HiZ));
	return FReply::Handled();
}

UWorld* SCrowdyGridView::ResolveVizWorld(bool& bOutIsEditorWorld)
{
	bOutIsEditorWorld = false;
	// Prefer the play world the user is looking at; fall back to the editor world for authoring.
	if (UWorld* PieWorld = FindRunningGameWorld())
	{
		return PieWorld;
	}
	if (GEditor)
	{
		bOutIsEditorWorld = true;
		return GEditor->GetEditorWorldContext().World();
	}
	return nullptr;
}

void SCrowdyGridView::DrawChunkBox(UWorld* World, const FStudioChunk& Low, const FStudioChunk& High, double ChunkSize, const FColor& Color, float Thickness, const FString& Label)
{
	if (!World)
	{
		return;
	}
	const int64 LoX = FMath::Min(Low.X, High.X), HiX = FMath::Max(Low.X, High.X);
	const int64 LoY = FMath::Min(Low.Y, High.Y), HiY = FMath::Max(Low.Y, High.Y);
	const int64 LoZ = FMath::Min(Low.Z, High.Z), HiZ = FMath::Max(Low.Z, High.Z);

	// A chunk at coord C covers world [C*size, (C+1)*size); a grid covers Low..High inclusive.
	const FVector WorldMin(LoX * ChunkSize, LoY * ChunkSize, LoZ * ChunkSize);
	const FVector WorldMax((HiX + 1) * ChunkSize, (HiY + 1) * ChunkSize, (HiZ + 1) * ChunkSize);
	const FVector Center = (WorldMin + WorldMax) * 0.5;
	const FVector Extent = (WorldMax - WorldMin) * 0.5;

	// Persistent so a non-realtime editor viewport keeps showing them between redraws; every
	// RefreshGridVisualization clears and re-draws from scratch. Outer boundary of the whole grid (thick).
	DrawDebugBox(World, Center, Extent, Color, /*bPersistent*/ true, /*LifeTime*/ -1.0f, /*DepthPriority*/ 0, Thickness);

	// Per-chunk separators (thinner) so a multi-chunk grid reads as its constituent chunks rather than
	// one box. Capped: separators only help for a modest count, and a huge grid would flood the viewport.
	const int64 CountX = HiX - LoX + 1, CountY = HiY - LoY + 1, CountZ = HiZ - LoZ + 1;
	const int64 MaxSeparatorCells = 512;
	if (CountX <= MaxSeparatorCells && CountY <= MaxSeparatorCells && CountZ <= MaxSeparatorCells
		&& CountX * CountY * CountZ > 1 && CountX * CountY * CountZ <= MaxSeparatorCells)
	{
		const float CellThickness = FMath::Max(1.0f, Thickness * 0.4f);
		const FVector CellExtent(ChunkSize * 0.5, ChunkSize * 0.5, ChunkSize * 0.5);
		for (int64 X = LoX; X <= HiX; ++X)
		{
			for (int64 Y = LoY; Y <= HiY; ++Y)
			{
				for (int64 Z = LoZ; Z <= HiZ; ++Z)
				{
					const FVector CellCenter((X + 0.5) * ChunkSize, (Y + 0.5) * ChunkSize, (Z + 0.5) * ChunkSize);
					DrawDebugBox(World, CellCenter, CellExtent, Color, /*bPersistent*/ true, /*LifeTime*/ -1.0f, /*DepthPriority*/ 0, CellThickness);
				}
			}
		}
	}

	if (!Label.IsEmpty())
	{
		DrawDebugString(World, FVector(Center.X, Center.Y, WorldMax.Z + 50.0), Label, nullptr, Color, /*Duration*/ -1.0f, /*bDrawShadow*/ true);
	}
}

bool SCrowdyGridView::TryGetCornerChunks(FStudioChunk& OutLow, FStudioChunk& OutHigh) const
{
	auto IsEmpty = [](const TSharedPtr<SEditableTextBox>& Box)
	{
		return !Box.IsValid() || Box->GetText().ToString().TrimStartAndEnd().IsEmpty();
	};

	// Unset = every corner field is blank. A typed value (including 0) is intentional, so a grid
	// authored at chunk [0,0,0]->[0,0,0] previews correctly instead of being mistaken for the default.
	if (IsEmpty(Corner1X) && IsEmpty(Corner1Y) && IsEmpty(Corner1Z) &&
		IsEmpty(Corner2X) && IsEmpty(Corner2Y) && IsEmpty(Corner2Z))
	{
		return false;
	}

	const int64 C1X = ParseInt(Corner1X), C1Y = ParseInt(Corner1Y), C1Z = ParseInt(Corner1Z);
	const int64 C2X = ParseInt(Corner2X), C2Y = ParseInt(Corner2Y), C2Z = ParseInt(Corner2Z);
	OutLow.X = FMath::Min(C1X, C2X); OutLow.Y = FMath::Min(C1Y, C2Y); OutLow.Z = FMath::Min(C1Z, C2Z);
	OutHigh.X = FMath::Max(C1X, C2X); OutHigh.Y = FMath::Max(C1Y, C2Y); OutHigh.Z = FMath::Max(C1Z, C2Z);
	return true;
}

bool SCrowdyGridView::TryGetSelectionChunks(FStudioChunk& OutLow, FStudioChunk& OutHigh, double ChunkSize) const
{
	if (!GEditor || ChunkSize <= 0.0)
	{
		return false;
	}

	TArray<AActor*> Selected;
	GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Selected);
	if (Selected.Num() == 0)
	{
		return false;
	}

	// World -> chunk is floor(coord / chunkSize), matching UHelperFunctions::GetChunkCoordinateAtLocation.
	auto ToChunk = [ChunkSize](double WorldCoord) -> int64
	{
		return static_cast<int64>(FMath::FloorToDouble(WorldCoord / ChunkSize));
	};

	// Accumulate the low/high chunk box across every selected actor's contributing chunk(s).
	bool bAny = false;
	int64 LoX = 0, LoY = 0, LoZ = 0, HiX = 0, HiY = 0, HiZ = 0;
	auto Accumulate = [&](int64 X, int64 Y, int64 Z)
	{
		if (!bAny)
		{
			LoX = HiX = X; LoY = HiY = Y; LoZ = HiZ = Z;
			bAny = true;
			return;
		}
		LoX = FMath::Min(LoX, X); HiX = FMath::Max(HiX, X);
		LoY = FMath::Min(LoY, Y); HiY = FMath::Max(HiY, Y);
		LoZ = FMath::Min(LoZ, Z); HiZ = FMath::Max(HiZ, Z);
	};

	for (const AActor* Actor : Selected)
	{
		if (!IsValid(Actor))
		{
			continue;
		}
		if (bSelectionByLocation)
		{
			// Like the runtime: the actor lives in the one chunk its pivot is in, regardless of size or
			// where it sits relative to chunk boundaries.
			const FVector Loc = Actor->GetActorLocation();
			Accumulate(ToChunk(Loc.X), ToChunk(Loc.Y), ToChunk(Loc.Z));
		}
		else
		{
			// Cover every chunk the actor's collision footprint touches. Colliding-only (true) so range
			// spheres, editor billboards and other non-colliding components don't inflate the grid.
			FVector Origin, Extent;
			Actor->GetActorBounds(/*bOnlyCollidingComponents*/ true, Origin, Extent);
			Accumulate(ToChunk(Origin.X - Extent.X), ToChunk(Origin.Y - Extent.Y), ToChunk(Origin.Z - Extent.Z));
			Accumulate(ToChunk(Origin.X + Extent.X), ToChunk(Origin.Y + Extent.Y), ToChunk(Origin.Z + Extent.Z));
		}
	}

	if (!bAny)
	{
		return false;
	}

	OutLow.X = LoX; OutLow.Y = LoY; OutLow.Z = LoZ;
	OutHigh.X = HiX; OutHigh.Y = HiY; OutHigh.Z = HiZ;
	return true;
}

void SCrowdyGridView::RefreshGridVisualization()
{
	bool bEditorWorld = false;
	UWorld* World = ResolveVizWorld(bEditorWorld);
	if (!World)
	{
		return;
	}

	// Clear our previous draw, then re-issue from scratch.
	FlushPersistentDebugLines(World);
	FlushDebugStrings(World);

	if (bShowGridViz)
	{
		bool bLive = false;
		const double ChunkSize = ResolveChunkSize(bLive);

		// Existing grids from the last scan (cyan).
		if (Controller.IsValid())
		{
			for (const TSharedPtr<FStudioGrid>& Grid : Controller->GetNearbyGrids())
			{
				if (Grid.IsValid())
				{
					DrawChunkBox(World, Grid->Low, Grid->High, ChunkSize, FColor(0, 200, 255), 5.0f,
						FString::Printf(TEXT("grid #%lld"), Grid->GridId));
				}
			}
		}

		// The corner-field grid (red) and the current selection's grid (orange) draw independently, so
		// you can author and preview both at the same time.
		FStudioChunk CornerLow, CornerHigh;
		if (TryGetCornerChunks(CornerLow, CornerHigh))
		{
			DrawChunkBox(World, CornerLow, CornerHigh, ChunkSize, FColor(235, 30, 30), 8.0f, TEXT("grid from corners"));
		}

		FStudioChunk SelLow, SelHigh;
		if (TryGetSelectionChunks(SelLow, SelHigh, ChunkSize))
		{
			DrawChunkBox(World, SelLow, SelHigh, ChunkSize, FColor(255, 165, 0), 6.0f, TEXT("grid from selection"));
		}
	}

	// A non-realtime editor viewport only shows the lines if we ask it to repaint.
	if (bEditorWorld && GEditor)
	{
		GEditor->RedrawLevelEditingViewports();
	}
}

void SCrowdyGridView::ClearGridVisualization()
{
	// Clear in both the running and editor worlds so nothing lingers after a toggle-off or teardown.
	if (UWorld* GameWorld = FindRunningGameWorld())
	{
		FlushPersistentDebugLines(GameWorld);
		FlushDebugStrings(GameWorld);
	}
	if (GEditor)
	{
		if (UWorld* EditorWorld = GEditor->GetEditorWorldContext().World())
		{
			FlushPersistentDebugLines(EditorWorld);
			FlushDebugStrings(EditorWorld);
			GEditor->RedrawLevelEditingViewports();
		}
	}
}

void SCrowdyGridView::SetShowGridViz(bool bEnable)
{
	bShowGridViz = bEnable;
	if (bEnable)
	{
		RefreshGridVisualization();
	}
	else
	{
		ClearGridVisualization();
	}
}

void SCrowdyGridView::HandlePieEvent(bool /*bIsSimulating*/)
{
	// PIE start/stop swaps the active world; redraw into whichever world is now in front.
	RefreshGridVisualization();
}

void SCrowdyGridView::HandleSelectionChanged(UObject* /*Object*/)
{
	// Picking different actors: redraw the "from selection" preview if its chunk box changed.
	MaybeRedrawForSelection();
}

void SCrowdyGridView::MaybeRedrawForSelection()
{
	if (!bShowGridViz)
	{
		return;
	}

	bool bLive = false;
	const double ChunkSize = ResolveChunkSize(bLive);
	FStudioChunk Low, High;
	const bool bHas = TryGetSelectionChunks(Low, High, ChunkSize);

	// Redraw only when the selection's chunk box changes: dragging an actor within its chunk, or
	// picking a different actor in the same chunk, costs nothing; crossing into a new chunk redraws.
	const bool bChanged = (bHas != bHadSelectionBox)
		|| (bHas && (Low.X != LastSelLow.X || Low.Y != LastSelLow.Y || Low.Z != LastSelLow.Z
			|| High.X != LastSelHigh.X || High.Y != LastSelHigh.Y || High.Z != LastSelHigh.Z));
	if (!bChanged)
	{
		return;
	}

	bHadSelectionBox = bHas;
	LastSelLow = Low;
	LastSelHigh = High;
	RefreshGridVisualization();
}

EActiveTimerReturnType SCrowdyGridView::PollSelectionForMove(double /*InCurrentTime*/, float /*InDeltaTime*/)
{
	MaybeRedrawForSelection();
	return EActiveTimerReturnType::Continue;
}

#undef LOCTEXT_NAMESPACE
