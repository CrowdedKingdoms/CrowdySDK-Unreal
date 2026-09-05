// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyApplyReviewPanel.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// Wide enough that an entity's name, its create-or-update word and the sentence explaining a refusal fit on one
	// line, and tall enough to show a plan of any size without the sheet leaving the screen. Both are limits rather
	// than sizes: the list scrolls past them, so a plan of six hundred changes takes the same room as one of six.
	// The names here are prefixed because the unity build merges this module's translation units and a bare helper
	// name would redefine another file's.
	constexpr float CrowdyApplyPanelWidth = 880.0f;
	constexpr float CrowdyApplyPanelMaxHeight = 620.0f;
	constexpr float CrowdyApplyAdditionsMaxHeight = 132.0f;
	constexpr float CrowdyApplyUnresolvableMaxHeight = 96.0f;

	const FName CrowdyApplyColumnTick("Tick");
	const FName CrowdyApplyColumnName("Name");
	const FName CrowdyApplyColumnState("State");
	const FName CrowdyApplyColumnWhy("Why");

	// The heading over one group of the list. An apply kind describes a pending upsert on a selection surface
	// rather than a value the server ever sends, so it is not one of the server nouns the vocabulary header owns.
	FText CrowdyApplyReviewKindHeading(ECrowdyApplyKind Kind)
	{
		switch (Kind)
		{
		case ECrowdyApplyKind::Attribute:  return LOCTEXT("ApplyGroupAttributes", "Attributes");
		case ECrowdyApplyKind::Function:   return LOCTEXT("ApplyGroupFunctions", "Functions");
		case ECrowdyApplyKind::Automation: return LOCTEXT("ApplyGroupAutomations", "Automations");
		case ECrowdyApplyKind::Trigger:    return LOCTEXT("ApplyGroupTriggers", "Triggers");
		default:                           return LOCTEXT("ApplyGroupModels", "Models");
		}
	}

	TSharedRef<SButton> CrowdyApplyReviewButton(const FText& Label, FOnClicked OnClicked)
	{
		const ISlateStyle& Style = FCrowdyStudioStyle::Get();
		return SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(10.0f, 5.0f))
			.OnClicked(OnClicked)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor::UseForeground())
			];
	}

	// One line of prose inside the additions or the unresolvable list.
	TSharedRef<SWidget> CrowdyApplyReviewLine(const FString& Text, const TCHAR* TextStyle)
	{
		return SNew(STextBlock)
			.TextStyle(&FCrowdyStudioStyle::Get(), TextStyle)
			.AutoWrapText(true)
			.Text(FText::FromString(Text));
	}
}

// One line of the review list. A multi-column row has to produce a widget per column: a row laying its own cells
// out in a horizontal box lines up with the header above it at exactly one width and drifts at every other one.
class SCrowdyApplyReviewUnitRow : public SMultiColumnTableRow<TSharedPtr<FCrowdyApplyReviewRow>>
{
public:
	SLATE_BEGIN_ARGS(SCrowdyApplyReviewUnitRow) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyApplyReviewRow>, Item)
		SLATE_EVENT(FOnCrowdyApplyRowToggled, OnToggled)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
	{
		Item = InArgs._Item;
		Toggled = InArgs._OnToggled;

		FSuperRowType::Construct(
			FSuperRowType::FArguments()
				.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
				.Padding(FMargin(0.0f, 1.0f)),
			OwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		if (!Item.IsValid())
		{
			return SNullWidget::NullWidget;
		}

		// Named apart from the table row's own Style member, which is a brush set rather than the Studio's style
		// set and which this would otherwise hide.
		const ISlateStyle& StudioStyle = FCrowdyStudioStyle::Get();

		if (Item->bHeading)
		{
			// A heading names the group under it and has nothing to say in the other columns. It carries no tick:
			// selection here is per entity, because the reason to hold something back is always about one entity.
			if (ColumnName != CrowdyApplyColumnName)
			{
				return SNullWidget::NullWidget;
			}

			return SNew(SBox)
				.VAlign(VAlign_Center)
				.Padding(FMargin(4.0f, 9.0f, 6.0f, 3.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->Heading))
					.TextStyle(&StudioStyle, "Crowdy.Text.BodyStrong")
				];
		}

		if (ColumnName == CrowdyApplyColumnTick)
		{
			const TWeakPtr<FCrowdyApplyReviewRow> WeakItem = Item;
			const FOnCrowdyApplyRowToggled OnToggled = Toggled;

			return SNew(SBox)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.ToolTipText(Item->bSelectable ? FText::GetEmpty() : FText::FromString(Item->Reason))
				[
					SNew(SCheckBox)
					.IsEnabled(Item->bSelectable)
					.IsChecked_Lambda([WeakItem]()
					{
						const TSharedPtr<FCrowdyApplyReviewRow> Row = WeakItem.Pin();
						return Row.IsValid() && Row->bChecked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([WeakItem, OnToggled](ECheckBoxState NewState)
					{
						const TSharedPtr<FCrowdyApplyReviewRow> Row = WeakItem.Pin();
						if (!Row.IsValid())
						{
							return;
						}
						Row->bChecked = NewState == ECheckBoxState::Checked;
						OnToggled.ExecuteIfBound(Row);
					})
				];
		}

		if (ColumnName == CrowdyApplyColumnName)
		{
			const FText Value = FText::FromString(Item->Unit.Display);
			return SNew(SBox)
				.VAlign(VAlign_Center)
				.Padding(FMargin(6.0f, 5.0f))
				[
					SNew(STextBlock)
					.Text(Value)
					.ToolTipText(Value)
					.TextStyle(&StudioStyle, "Crowdy.Text.Body")
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				];
		}

		if (ColumnName == CrowdyApplyColumnState)
		{
			return SNew(SBox)
				.VAlign(VAlign_Center)
				.Padding(FMargin(6.0f, 5.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Item->StateWord))
					.TextStyle(&StudioStyle, "Crowdy.Text.Subtle")
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				];
		}

		if (ColumnName == CrowdyApplyColumnWhy)
		{
			// Empty on every entity that may be ticked, so the only ink in this column is on the rows that cannot
			// be acted on and say why.
			const FText Value = FText::FromString(Item->Reason);
			return SNew(SBox)
				.VAlign(VAlign_Center)
				.Padding(FMargin(6.0f, 5.0f))
				[
					SNew(STextBlock)
					.Text(Value)
					.ToolTipText(Value)
					.TextStyle(&StudioStyle, "Crowdy.Text.Subtle")
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				];
		}

		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FCrowdyApplyReviewRow> Item;
	FOnCrowdyApplyRowToggled Toggled;
};

void SCrowdyApplyReviewPanel::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	Dismiss = InArgs._OnDismiss;

	if (Controller.IsValid())
	{
		// A re-plan replaces every pending array, so what is on screen describes a plan that no longer exists.
		Controller->OnSchemaSyncReportChanged.AddSP(this, &SCrowdyApplyReviewPanel::HandleSchemaSyncReportChanged);
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyApplyReviewPanel::HandleAppChanged);
	}

	// The app this panel believes it is showing, taken as it opened. The LIVE selection, not the app the pending
	// plan is pinned to: the plan carries that itself, and taking it here would make the send's guard compare the
	// plan against a copy of its own app. It would also read zero on a panel opened while a plan is still running,
	// and a zero refuses every later send with a sentence about a different app that is not what went wrong.
	ExpectedAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SHeaderRow> Header = SNew(SHeaderRow).Style(&Style, "Crowdy.HeaderRow");

	Header->AddColumn(SHeaderRow::Column(CrowdyApplyColumnTick).DefaultLabel(FText::GetEmpty()).FixedWidth(32.0f));
	Header->AddColumn(SHeaderRow::Column(CrowdyApplyColumnName)
		.DefaultLabel(LOCTEXT("ApplyColumnName", "Change")).FillWidth(0.44f));
	Header->AddColumn(SHeaderRow::Column(CrowdyApplyColumnState)
		.DefaultLabel(LOCTEXT("ApplyColumnState", "Does")).FixedWidth(78.0f));
	Header->AddColumn(SHeaderRow::Column(CrowdyApplyColumnWhy)
		.DefaultLabel(LOCTEXT("ApplyColumnWhy", "Note")).FillWidth(0.40f));

	TSharedRef<SWidget> Sheet = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SAssignNew(HeadlineText, STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Heading")
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SAssignNew(AppLineText, STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SAssignNew(CountsText, STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Body")
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SAssignNew(BlockedReasonText, STextBlock)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
			.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
			.AutoWrapText(true)
			.Visibility(EVisibility::Collapsed)
		]

		// What could not be identified, named rather than counted, so the reader knows which effect to fix.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SBox)
			.MaxDesiredHeight(CrowdyApplyUnresolvableMaxHeight)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SAssignNew(UnresolvableBox, SVerticalBox)
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SAssignNew(NoticeText, STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
			.Visibility(EVisibility::Collapsed)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth()
			[
				CrowdyApplyReviewButton(LOCTEXT("ApplyClose", "Close"),
					FOnClicked::CreateSP(this, &SCrowdyApplyReviewPanel::OnCloseClicked))
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SSpacer)
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				// Built NON-FOCUSABLE, and nothing here ever moves keyboard focus to it, so Enter pressed anywhere
				// on the page cannot fire it.
				SAssignNew(SendButton, SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Primary")
				.ContentPadding(FMargin(13.0f, 7.0f))
				.IsFocusable(false)
				.IsEnabled(this, &SCrowdyApplyReviewPanel::IsSendEnabled)
				.OnClicked(this, &SCrowdyApplyReviewPanel::OnSendClicked)
				[
					SAssignNew(SendLabelText, STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		],
		FMargin(14.0f, 12.0f));

	ChildSlot
	[
		SNew(SBox)
		.WidthOverride(CrowdyApplyPanelWidth)
		.MaxDesiredHeight(CrowdyApplyPanelMaxHeight)
		.Padding(FMargin(12.0f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.TextStyle(&Style, "Crowdy.Text.Subtle")
					.AutoWrapText(true)
					.Text(LOCTEXT("ApplyPickHint",
						"Everything is ticked. Untick anything you are not ready to send; whatever the rest needs is added back for you and listed above the sheet."))
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 6.0f, 0.0f)
				[
					CrowdyApplyReviewButton(LOCTEXT("ApplySelectAll", "Tick everything"),
						FOnClicked::CreateSP(this, &SCrowdyApplyReviewPanel::OnSelectAllClicked))
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					CrowdyApplyReviewButton(LOCTEXT("ApplyClearAll", "Untick everything"),
						FOnClicked::CreateSP(this, &SCrowdyApplyReviewPanel::OnClearAllClicked))
				]
			]

			// Added on the reader's behalf, each naming the entity that forced it, above the list they picked from.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(AdditionsHeaderText, STextBlock)
					.TextStyle(&Style, "Crowdy.Text.BodyStrong")
					.AutoWrapText(true)
					.Visibility(EVisibility::Collapsed)
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.MaxDesiredHeight(CrowdyApplyAdditionsMaxHeight)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SAssignNew(AdditionsBox, SVerticalBox)
						]
					]
				]
			]

			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				CrowdyStudioWidgets::Card(
					SAssignNew(UnitListView, SListView<TSharedPtr<FCrowdyApplyReviewRow>>)
					.ListViewStyle(&Style, "Crowdy.TableView")
					.ListItemsSource(&UnitRows)
					.OnGenerateRow(this, &SCrowdyApplyReviewPanel::MakeUnitRow)
					// Nothing here acts on a highlighted row: the tick is the choice, and a second notion of
					// "the current row" would be a second answer about what this sends.
					.SelectionMode(ESelectionMode::None)
					.HeaderRow(Header),
					FMargin(4.0f), /*bFlat*/ true)
			]

			// The sheet sits outside the scrolling half on purpose: a button reading "Send 47 changes" is a
			// confirmation only for as long as the 47 is still on screen beside it.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				Sheet
			]
		]
	];

	RebuildFromController(/*bAnnounceReplan*/ false);
}

SCrowdyApplyReviewPanel::~SCrowdyApplyReviewPanel()
{
	if (Controller.IsValid())
	{
		Controller->OnSchemaSyncReportChanged.RemoveAll(this);
		Controller->OnSelectedAppChanged.RemoveAll(this);
	}
}

TArray<FString> SCrowdyApplyReviewPanel::GatherSelectionKeys(const FCrowdyApplyPlanInput& Input) const
{
	if (!bSelectionIsAll)
	{
		return SelectedKeys;
	}

	// Everything means the WHOLE plan, wiring included. The list hides the revision attribute and the touch
	// function the SDK provisions for each model, so a model whose only pending changes are that pair has no row to
	// tick at all, and taking the ticked rows here would leave it unsent for as long as the pair was all it had.
	//
	// Everything that CAN be picked, though. A unit whose owning model nothing could name is refused as a pick and
	// is drawn here with its tick disabled, so naming it would build a set the closure then refuses: the send
	// control would be dead with every row ticked and arm the moment any unrelated row was unticked. It still takes
	// part in the closure as a candidate, so a selection that genuinely depends on it is still refused.
	const TArray<FCrowdyApplyUnit> AllUnits = CrowdyApplySelection::BuildUnits(Input);

	TArray<FString> Keys;
	Keys.Reserve(AllUnits.Num());
	for (const FCrowdyApplyUnit& Unit : AllUnits)
	{
		FString UnusedReason;
		if (CrowdyApplySelection::CanSelect(Unit, UnusedReason))
		{
			Keys.Add(Unit.IdentityKey());
		}
	}
	return Keys;
}

void SCrowdyApplyReviewPanel::RebuildFromController(bool bAnnounceReplan)
{
	if (!Controller.IsValid())
	{
		return;
	}

	// Held for the length of this call and no longer: it points into the controller's five pending arrays, and a
	// re-plan replaces every one of them.
	const FCrowdyApplyPlanInput Input = Controller->MakeSchemaApplyPlanInput();

	const TArray<FCrowdyApplyUnit> AllUnits = CrowdyApplySelection::BuildUnits(Input);
	const TArray<FCrowdyApplyUnit> Selectable = CrowdyApplySelection::SelectableUnits(AllUnits);

	// Built from the selection as it stood, so the plan itself reports which of those keys this plan no longer
	// holds rather than the survivors being computed here and the loss going unmentioned.
	Plan = CrowdyApplySelection::BuildPlan(Input, GatherSelectionKeys(Input));
	ShownConsentToken = Plan.ConsentToken;

	int32 Dropped = 0;
	if (!bSelectionIsAll)
	{
		SelectedKeys = Plan.SelectedKeys;
		Dropped = Plan.DroppedKeys.Num();
	}

	// One sentence for both things this rebuild can have changed under the reader: the plan itself being replaced,
	// and part of what they picked no longer being in the new one. A partial drop is not a refusal, so the count of
	// what went is the part they have to know rather than a reason to stop.
	if (bAnnounceReplan && Dropped > 0)
	{
		SetNotice(FText::Format(
			LOCTEXT("ApplyReplannedAndDropped",
				"This plan was worked out again while the sheet was on screen, so what is listed here has changed. {0} of what you picked is no longer in it and was dropped. Read it again before you send."),
			FText::AsNumber(Dropped)));
	}
	else if (bAnnounceReplan)
	{
		SetNotice(LOCTEXT("ApplyReplanned",
			"This plan was worked out again while the sheet was on screen, so what is listed here has changed. Read it again before you send."));
	}
	else if (Dropped > 0)
	{
		SetNotice(FText::Format(
			LOCTEXT("ApplySelectionDropped",
				"{0} of what you picked is no longer in this plan and was dropped."),
			FText::AsNumber(Dropped)));
	}

	RebuildUnitRows(Selectable);
	RebuildAdditions();
	RebuildSheet();
}

void SCrowdyApplyReviewPanel::RefreshPlanFromSelection()
{
	if (!Controller.IsValid())
	{
		return;
	}

	const FCrowdyApplyPlanInput Input = Controller->MakeSchemaApplyPlanInput();
	Plan = CrowdyApplySelection::BuildPlan(Input, GatherSelectionKeys(Input));
	ShownConsentToken = Plan.ConsentToken;

	RebuildAdditions();
	RebuildSheet();
}

void SCrowdyApplyReviewPanel::RebuildUnitRows(const TArray<FCrowdyApplyUnit>& Selectable)
{
	UnitRows.Reset();

	// Grouped by kind in the order the apply runs, so the list reads in the order the server is written in and a
	// prerequisite is always above the thing that needs it.
	for (const ECrowdyApplyKind Kind : CrowdyApplySelection::ApplyOrder())
	{
		bool bHeadingWritten = false;

		for (const FCrowdyApplyUnit& Unit : Selectable)
		{
			if (Unit.Kind != Kind)
			{
				continue;
			}

			if (!bHeadingWritten)
			{
				TSharedRef<FCrowdyApplyReviewRow> Heading = MakeShared<FCrowdyApplyReviewRow>();
				Heading->bHeading = true;
				Heading->Heading = CrowdyApplyReviewKindHeading(Kind).ToString();
				UnitRows.Add(Heading);
				bHeadingWritten = true;
			}

			TSharedRef<FCrowdyApplyReviewRow> Row = MakeShared<FCrowdyApplyReviewRow>();
			Row->Unit = Unit;
			Row->IdentityKey = Unit.IdentityKey();
			Row->StateWord = Unit.bIsNew
				? LOCTEXT("ApplyStateCreate", "create").ToString()
				: LOCTEXT("ApplyStateUpdate", "update").ToString();

			// Whether this entity may be ticked at all is the selection layer's answer, and so is the sentence for
			// when it may not: a row nobody can act on has to say why where the tick would have been.
			FString Reason;
			Row->bSelectable = CrowdyApplySelection::CanSelect(Unit, Reason);
			Row->Reason = Reason;

			// An entity that cannot be ticked is never carried as ticked either, whatever the selection said: the
			// closure refuses a plan that needs it, and showing it as chosen would suggest a send that cannot run.
			Row->bChecked = Row->bSelectable
				&& (bSelectionIsAll || SelectedKeys.ContainsByPredicate(
					[&Row](const FString& Key) { return Key.Equals(Row->IdentityKey, ESearchCase::CaseSensitive); }));

			UnitRows.Add(Row);
		}
	}

	if (UnitListView.IsValid())
	{
		UnitListView->RequestListRefresh();
	}
}

void SCrowdyApplyReviewPanel::RebuildAdditions()
{
	if (AdditionsHeaderText.IsValid())
	{
		AdditionsHeaderText->SetText(FText::FromString(Plan.Sheet.AddedLine));
		AdditionsHeaderText->SetVisibility(
			Plan.Sheet.AddedLine.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}

	if (!AdditionsBox.IsValid())
	{
		return;
	}

	AdditionsBox->ClearChildren();

	for (const FCrowdyApplyAddition& Addition : Plan.Additions)
	{
		AdditionsBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
		[
			// The sentence is the selection layer's, naming both ends and the field that connects them. Nothing
			// here rewrites it: a reader acts on "you ticked this, so this had to come too", and only the layer
			// that worked out the connection can say which end was which.
			CrowdyApplyReviewLine(Addition.Unit.Display + TEXT("   ") + Addition.Because, TEXT("Crowdy.Text.Subtle"))
		];
	}
}

void SCrowdyApplyReviewPanel::RebuildSheet()
{
	const FCrowdyApplySheet& Sheet = Plan.Sheet;

	if (HeadlineText.IsValid())
	{
		HeadlineText->SetText(FText::FromString(Sheet.Headline));
	}
	if (AppLineText.IsValid())
	{
		AppLineText->SetText(FText::FromString(Sheet.AppLine));
	}
	if (SendLabelText.IsValid())
	{
		SendLabelText->SetText(FText::FromString(Sheet.ActionLabel));
	}

	if (CountsText.IsValid())
	{
		// Joined once, here, rather than in a bound attribute: none of it changes between two painted frames.
		FString Lines;
		auto Append = [&Lines](const FString& Line)
		{
			if (!Line.IsEmpty())
			{
				Lines += Lines.IsEmpty() ? Line : LINE_TERMINATOR + Line;
			}
		};

		for (const FString& Line : Sheet.CountLines)
		{
			Append(Line);
		}
		Append(Sheet.ReservedLine);
		Append(Sheet.RemainderLine);

		CountsText->SetText(FText::FromString(Lines));
		CountsText->SetVisibility(Lines.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}

	if (BlockedReasonText.IsValid())
	{
		BlockedReasonText->SetText(FText::FromString(Sheet.BlockedReason));
		BlockedReasonText->SetVisibility(
			Sheet.BlockedReason.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}

	if (UnresolvableBox.IsValid())
	{
		UnresolvableBox->ClearChildren();
		for (const FCrowdyApplyUnit& Unit : Plan.Unresolvable)
		{
			UnresolvableBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				CrowdyApplyReviewLine(TEXT("- ") + Unit.Display, TEXT("Crowdy.Text.Subtle"))
			];
		}
	}
}

void SCrowdyApplyReviewPanel::CaptureSelectionFromRows()
{
	SelectedKeys.Reset();

	int32 Selectable = 0;
	for (const TSharedPtr<FCrowdyApplyReviewRow>& Row : UnitRows)
	{
		if (!Row.IsValid() || Row->bHeading || !Row->bSelectable)
		{
			continue;
		}

		++Selectable;
		if (Row->bChecked)
		{
			SelectedKeys.Add(Row->IdentityKey);
		}
	}

	// Every tickable row ticked is the same state as never having touched one, and it has to be, because
	// "everything" means the whole plan while a hand-built list of every row means only the rows: a model whose
	// only pending changes are the wiring the SDK keeps has no row here and would otherwise go unsent.
	bSelectionIsAll = Selectable > 0 && SelectedKeys.Num() == Selectable;
}

void SCrowdyApplyReviewPanel::HandleRowToggled(TSharedPtr<FCrowdyApplyReviewRow> Row)
{
	CaptureSelectionFromRows();

	// The list itself does not move when a tick does; what a tick changes is the closure, the additions and the
	// sheet, so only those are rebuilt.
	SetNotice(FText::GetEmpty());
	RefreshPlanFromSelection();
}

FReply SCrowdyApplyReviewPanel::OnSelectAllClicked()
{
	for (const TSharedPtr<FCrowdyApplyReviewRow>& Row : UnitRows)
	{
		if (Row.IsValid() && !Row->bHeading && Row->bSelectable)
		{
			Row->bChecked = true;
		}
	}

	CaptureSelectionFromRows();
	SetNotice(FText::GetEmpty());
	RefreshPlanFromSelection();
	return FReply::Handled();
}

FReply SCrowdyApplyReviewPanel::OnClearAllClicked()
{
	for (const TSharedPtr<FCrowdyApplyReviewRow>& Row : UnitRows)
	{
		if (Row.IsValid() && !Row->bHeading)
		{
			Row->bChecked = false;
		}
	}

	SelectedKeys.Reset();
	bSelectionIsAll = false;

	SetNotice(FText::GetEmpty());
	RefreshPlanFromSelection();
	return FReply::Handled();
}

FReply SCrowdyApplyReviewPanel::OnCloseClicked()
{
	Dismiss.ExecuteIfBound();
	return FReply::Handled();
}

bool SCrowdyApplyReviewPanel::IsSendEnabled() const
{
	return Controller.IsValid() && Plan.bSendable;
}

FReply SCrowdyApplyReviewPanel::OnSendClicked()
{
	if (!Controller.IsValid())
	{
		return FReply::Handled();
	}

	// TWO SOURCES, which is the whole point. The left side is gathered FROM THE CONTROLLER at the moment of the
	// click and closed afresh; the right side is the fingerprint of the plan the sheet was drawn from. Rebuilding
	// from the plan already held here would re-derive the same answer and agree with itself however far the
	// pending plan had moved, which is a guard that gates nothing.
	const FCrowdyApplyPlanInput FreshInput = Controller->MakeSchemaApplyPlanInput();
	const FCrowdyApplyPlan Fresh = CrowdyApplySelection::BuildPlan(FreshInput, GatherSelectionKeys(FreshInput));

	if (!Fresh.ConsentToken.Equals(ShownConsentToken, ESearchCase::CaseSensitive))
	{
		// Said here in stronger words than a plain re-plan gets, because this press did nothing and the reader has
		// to know that before anything else.
		RebuildFromController(/*bAnnounceReplan*/ false);
		SetNotice(LOCTEXT("ApplyPlanMoved",
			"What this would send changed while the sheet was on screen, so nothing was sent. Read it again, then press Send."));
		return FReply::Handled();
	}

	if (!Fresh.bSendable)
	{
		return FReply::Handled();
	}

	// ExpectedAppId is the app this panel opened against, which is not necessarily the one selected at this
	// instant. The controller compares the plan's own app, this one and the app the pending arrays are pinned to
	// against the live selection, so the check has four values from three sources rather than one agreeing with
	// itself.
	if (!Controller->ApplySchemaSelection(Fresh, ExpectedAppId))
	{
		// Nothing was sent, so the sheet stays. Dismissing here would throw away a reviewed selection over a press
		// that did not write, and leave the refusal explaining a page the reader can no longer see.
		SetNotice(LOCTEXT("ApplyRefused",
			"Nothing was sent. The status message says why; what you picked is still here."));
		return FReply::Handled();
	}

	Dismiss.ExecuteIfBound();
	return FReply::Handled();
}

TSharedRef<ITableRow> SCrowdyApplyReviewPanel::MakeUnitRow(
	TSharedPtr<FCrowdyApplyReviewRow> Row, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SCrowdyApplyReviewUnitRow, OwnerTable)
		.Item(Row)
		.OnToggled(FOnCrowdyApplyRowToggled::CreateSP(this, &SCrowdyApplyReviewPanel::HandleRowToggled));
}

void SCrowdyApplyReviewPanel::SetNotice(const FText& Message)
{
	if (!NoticeText.IsValid())
	{
		return;
	}

	NoticeText->SetText(Message);
	NoticeText->SetVisibility(Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
}

void SCrowdyApplyReviewPanel::HandleSchemaSyncReportChanged()
{
	// The pending arrays this panel describes have been replaced. Following them keeps the sheet a description of
	// what is actually pending; the consent token still guards the gap between this rebuild and the next click.
	//
	// Announced, because this rebuild also refreshes that token and so re-arms the send control. Re-arming in
	// silence hands the reader a live write against a sheet whose contents changed while they were reading it.
	RebuildFromController(/*bAnnounceReplan*/ true);
}

void SCrowdyApplyReviewPanel::HandleAppChanged()
{
	// Everything on screen names entities of the app the plan was computed for. Rather than render a sheet about
	// somewhere the reader is no longer looking, this goes away and the next press opens a fresh one.
	Dismiss.ExecuteIfBound();
}

#undef LOCTEXT_NAMESPACE
