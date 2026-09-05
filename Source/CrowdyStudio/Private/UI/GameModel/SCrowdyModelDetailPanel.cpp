// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyModelDetailPanel.h"

#include "GameModel/CrowdyGameModelDelete.h"
#include "GameModel/CrowdyModelSnapshot.h" // FCrowdyModelSnapshot::AttributeAuthoredNames
#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/GameModel/CrowdyModelCodeLink.h"
#include "UI/GameModel/SCrowdyModelSectionTable.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// A neutral button carrying one word. The Models page spends no brand colour, so the secondary style is the
	// strongest a control on it gets.
	TSharedRef<SWidget> CrowdyModelOpenButton(const FText& Label, const FText& Tooltip, FOnClicked OnClicked,
		TAttribute<bool> Enabled)
	{
		const ISlateStyle& Style = FCrowdyStudioStyle::Get();
		return SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(10.0f, 4.0f))
			.ToolTipText(Tooltip)
			.IsEnabled(Enabled)
			.OnClicked(OnClicked)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[ CrowdyStudioWidgets::Icon(TEXT("external-link"), 12.0f, FSlateColor::UseForeground()) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Label)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			];
	}
}

FString SCrowdyModelDetailPanel::CountPhrase(
	ECrowdyModelLoadState State, int32 Count, const TCHAR* Family, const TCHAR* Singular, const TCHAR* Plural)
{
	switch (State)
	{
	case ECrowdyModelLoadState::Loaded:
		return CountPhrase(Count, Singular, Plural);
	case ECrowdyModelLoadState::Loading:
		return FString::Printf(TEXT("%s loading"), Family);
	case ECrowdyModelLoadState::Failed:
		// The one thing this line must never do is read as a read still in progress. A failed read never repaints,
		// so "loading" here would sit there forever describing something that stopped.
		return FString::Printf(TEXT("%s could not be read"), Family);
	default:
		return FString::Printf(TEXT("%s not read"), Family);
	}
}

FString SCrowdyModelDetailPanel::EntityKey(const FCrowdyModelRow& Row)
{
	// A separator no server key can contain, so two entities cannot join into one string that matches a third.
	return FString::Printf(TEXT("%d|%s|%s"),
		static_cast<int32>(Row.Kind), *Row.OwningType, *Row.Name);
}

int32 SCrowdyModelDetailPanel::SectionIndex(const FString& SectionKey)
{
	if (SectionKey == TEXT("functions"))
	{
		return 1;
	}
	if (SectionKey == TEXT("automations"))
	{
		return 2;
	}
	return 0;
}

FString SCrowdyModelDetailPanel::CountPhrase(int32 Count, const TCHAR* Singular, const TCHAR* Plural)
{
	return FString::Printf(TEXT("%d %s"), Count, Count == 1 ? Singular : Plural);
}

void SCrowdyModelDetailPanel::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	IsRowMarked = InArgs._IsRowMarked;
	ToggleRowMark = InArgs._OnToggleRowMark;
	ShowLink = InArgs._OnShowLink;
	ActiveSection = TEXT("attributes");

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> Header = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SAssignNew(TitleText, STextBlock).TextStyle(&Style, "Crowdy.Text.Heading").OverflowPolicy(ETextOverflowPolicy::Ellipsis) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
			[
				CrowdyModelOpenButton(
					LOCTEXT("OpenModelClass", "Open"),
					LOCTEXT("OpenModelClassTip", "Open the class that declares this model. Disabled for a model no class in this project declares."),
					FOnClicked::CreateSP(this, &SCrowdyModelDetailPanel::OnOpenModelClicked),
					TAttribute<bool>::CreateLambda([this]() { return bCanOpenModel; }))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
		[ SAssignNew(SubtitleText, STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle") ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			// Capped and scrolled: a long description is the one thing on this pane that can grow without limit, and
			// the page it sits on does not scroll. A height cap on its own only reserves less room, it does not stop
			// the text being drawn, so without the clip and the scroll a long description paints straight over the
			// section tabs and the table header beneath it.
			SNew(SBox)
			.MaxDesiredHeight(64.0f)
			.Clipping(EWidgetClipping::ClipToBounds)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[ SAssignNew(DescriptionText, STextBlock).TextStyle(&Style, "Crowdy.Text.Body").AutoWrapText(true) ]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[ SAssignNew(CountsText, STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle") ],
		FMargin(14.0f, 12.0f));

	ChildSlot
	[
		SAssignNew(RootSwitcher, SWidgetSwitcher)

		+ SWidgetSwitcher::Slot()
		[
			CrowdyStudioWidgets::EmptyState(TEXT("cube"), LOCTEXT("NoModelSelected",
				"Select a model on the left to see its attributes, functions and automations."))
		]

		+ SWidgetSwitcher::Slot()
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight()
			[ Header ]

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom)
				[
					CrowdyStudioWidgets::TabStrip(
						{ TEXT("attributes"), TEXT("functions"), TEXT("automations") },
						{ LOCTEXT("SectionAttributes", "Attributes"), LOCTEXT("SectionFunctions", "Functions"), LOCTEXT("SectionAutomations", "Automations") },
						TAttribute<FString>::CreateSP(this, &SCrowdyModelDetailPanel::GetActiveSection),
						[this](const FString& SectionKey) { SetActiveSection(SectionKey); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					// Marking costs nothing and reads nothing: what a set of marks would do is worked out once,
					// when the review is opened. Disabled for a row the project declares, since deleting one of
					// those succeeds and the next sync puts it straight back.
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Secondary")
					.ContentPadding(FMargin(10.0f, 4.0f))
					.ToolTipText_Lambda([this]() { return RowMarkTooltip; })
					.IsEnabled_Lambda([this]() { return bRowMarkEnabled; })
					.OnClicked(this, &SCrowdyModelDetailPanel::OnMarkRowClicked)
					[
						SAssignNew(MarkRowLabelText, STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]

				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					CrowdyModelOpenButton(
						LOCTEXT("OpenSelectedRow", "Open selected"),
						LOCTEXT("OpenSelectedRowTip", "Open the class or the Crowdy Effect asset that declares the highlighted row. Disabled while nothing is highlighted, and for a row nothing in this project declares."),
						FOnClicked::CreateSP(this, &SCrowdyModelDetailPanel::OnOpenRowClicked),
						TAttribute<bool>::CreateLambda([this]() { return IsOpenRowEnabled(); }))
				]
			]

			// Why the highlighted row offers no delete, said where the control would have been rather than in a
			// dialog after the click. Empty, and hidden, for a row that may be deleted and while none is
			// highlighted.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SAssignNew(RowGateReasonText, STextBlock)
				.TextStyle(&Style, "Crowdy.Text.Subtle")
				.AutoWrapText(true)
				.Visibility(EVisibility::Collapsed)
			]

			// The automations arrived and their triggers did not. It is a line rather than a placeholder because
			// the automations rendered, so their table has rows and shows no placeholder at all, and without this
			// an automation with no schedule beside it is indistinguishable from one that has none.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SAssignNew(TriggerNoteText, STextBlock)
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Warning()))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.AutoWrapText(true)
				.Visibility(EVisibility::Collapsed)
			]

			// What else in this app names the highlighted row. One line always, and the list behind a disclosure,
			// because six lines permanently open would come out of the table below on a page that does not scroll.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SAssignNew(CrossLinkSummaryText, STextBlock)
						.TextStyle(&Style, "Crowdy.Text.Subtle")
						.AutoWrapText(true)
						.Visibility(EVisibility::Collapsed)
					]

					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SAssignNew(CrossLinkToggleBox, SButton)
						.ButtonStyle(&Style, "Crowdy.Button.Ghost")
						.ContentPadding(FMargin(6.0f, 3.0f))
						.Visibility(EVisibility::Collapsed)
						.OnClicked(this, &SCrowdyModelDetailPanel::OnToggleCrossLinks)
						[
							SAssignNew(CrossLinkToggleLabel, STextBlock)
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
							.ColorAndOpacity(FSlateColor::UseForeground())
						]
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SAssignNew(CrossLinkLinesBox, SVerticalBox)
					.Visibility(EVisibility::Collapsed)
				]
			]

			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				CrowdyStudioWidgets::Card(
					SAssignNew(SectionSwitcher, SWidgetSwitcher)

					+ SWidgetSwitcher::Slot()
					[
						SAssignNew(AttributesTable, SCrowdyModelSectionTable)
						.Controller(Controller)
						.Columns(CrowdyModelTableColumns::ModelSectionColumns(LOCTEXT("ColumnValueType", "Holds")))
						// Every one of these three is replaced the moment a model is shown. They start from the
						// same file the replacements come from so that nothing here spells an empty-state answer
						// of its own, not even one nobody can see.
						.Placeholder(FText::FromString(
							CrowdyModelEmptyState::AttributeTable(ECrowdyModelLoadState::NeverRequested)))
						.PlaceholderIcon(TEXT("config"))
						.OnSelectionChanged(this, &SCrowdyModelDetailPanel::OnSectionSelectionChanged)
					]

					+ SWidgetSwitcher::Slot()
					[
						SAssignNew(FunctionsTable, SCrowdyModelSectionTable)
						.Controller(Controller)
						.Columns(CrowdyModelTableColumns::ModelSectionColumns(LOCTEXT("ColumnReturns", "Gives back")))
						.Placeholder(FText::FromString(
							CrowdyModelEmptyState::FunctionTable(ECrowdyModelLoadState::NeverRequested)))
						.PlaceholderIcon(TEXT("wand"))
						.OnSelectionChanged(this, &SCrowdyModelDetailPanel::OnSectionSelectionChanged)
					]

					+ SWidgetSwitcher::Slot()
					[
						SAssignNew(AutomationsTable, SCrowdyModelSectionTable)
						.Controller(Controller)
						.Columns(CrowdyModelTableColumns::ModelSectionColumns(LOCTEXT("ColumnRunsWhen", "Runs")))
						.Placeholder(FText::FromString(
							CrowdyModelEmptyState::AutomationTable(ECrowdyModelLoadState::NeverRequested)))
						.PlaceholderIcon(TEXT("clock"))
						.OnSelectionChanged(this, &SCrowdyModelDetailPanel::OnSectionSelectionChanged)
					],
					FMargin(4.0f), /*bFlat*/ true)
			]
		]
	];

	RootSwitcher->SetActiveWidgetIndex(0);
	SectionSwitcher->SetActiveWidgetIndex(SectionIndex(ActiveSection));
	UpdateRowDeleteControl();
	UpdateTriggerNote();
	UpdateCrossLinks();
}

void SCrowdyModelDetailPanel::SetActiveSection(const FString& SectionKey)
{
	ActiveSection = SectionKey;
	if (SectionSwitcher.IsValid())
	{
		SectionSwitcher->SetActiveWidgetIndex(SectionIndex(SectionKey));
	}

	// Each section keeps its own highlight, so everything that answers for the highlighted row now answers for a
	// different one, or for none.
	UpdateRowDeleteControl();
	UpdateTriggerNote();
	UpdateCrossLinks();
}

void SCrowdyModelDetailPanel::SetModel(const FCrowdyModelSummary& InModel)
{
	Model = InModel;
	bHasModel = true;

	if (RootSwitcher.IsValid())
	{
		RootSwitcher->SetActiveWidgetIndex(1);
	}

	// Asked for once per model and no more. The controller keeps each model's attributes once it has read them,
	// and asking again would issue a fresh read every time the user came back to a model already open earlier.
	//
	// A model that exists only in the project is never asked for at all: the server has no such type, so the read
	// has nothing to return and the pane would wait on it forever. Everything that model is made of is already in
	// hand.
	if (Controller.IsValid()
		&& !Model.TypeName.IsEmpty()
		&& !Model.bCodeOnly
		&& Controller->GetPropertyDefsForType(Model.TypeName) == nullptr)
	{
		Controller->FetchPropertyDefs(Model.TypeName);
	}

	RefreshSections();
}

void SCrowdyModelDetailPanel::ClearModel()
{
	Model = FCrowdyModelSummary();
	bHasModel = false;
	bCanOpenModel = false;
	CheckedRowPath.Reset();
	bCheckedRowPathOpens = false;

	if (RootSwitcher.IsValid())
	{
		RootSwitcher->SetActiveWidgetIndex(0);
	}

	// Drop the rows as well as hiding them. They describe a model that is no longer open, and keeping them would
	// flash the previous model's contents for a frame the next time a model is selected.
	if (AttributesTable.IsValid())
	{
		AttributesTable->SetRows(TArray<TSharedPtr<FCrowdyModelRow>>());
	}
	if (FunctionsTable.IsValid())
	{
		FunctionsTable->SetRows(TArray<TSharedPtr<FCrowdyModelRow>>());
	}
	if (AutomationsTable.IsValid())
	{
		AutomationsTable->SetRows(TArray<TSharedPtr<FCrowdyModelRow>>());
	}

	UpdateRowDeleteControl();
	UpdateTriggerNote();
	UpdateCrossLinks();
}

void SCrowdyModelDetailPanel::RefreshSections()
{
	if (!bHasModel || !Controller.IsValid())
	{
		return;
	}

	const FString& TypeName = Model.TypeName;

	// The last finished plan for this app, or null when nobody has run one. Every row is classified against it, and
	// with none every row reads as unclassified, which is the honest answer rather than a guess.
	const TSharedPtr<const FCrowdyModelSnapshot> Snapshot = Controller->GetModelSnapshot();
	const FCrowdyModelSnapshot* Plan = Snapshot.Get();

	// Which of five situations this model's attributes are in. An empty table says "this model has none" only in
	// one of them, and claiming it in the other four reports a read nobody made, or one that failed, as a fact
	// about the design.
	ECrowdyModelLoadState AttributesState = ECrowdyModelLoadState::Loaded;

	// The entry that gathers what belongs to no model. Asked once and answered once, because every section on this
	// pane has to agree about which of the two things the reader is looking at.
	const bool bAppWide = TypeName.IsEmpty();

	if (bAppWide)
	{
		// This entry gathers what belongs to the app rather than to a model, and only a model has attributes. Not a
		// load state at all, so the table says which question does not apply here rather than picking one.
		AttributesTable->SetPlaceholder(FText::FromString(CrowdyModelEmptyState::AttributeTableAppWide()));
		AttributesTable->SetRows(TArray<TSharedPtr<FCrowdyModelRow>>());
	}
	else if (Model.bCodeOnly)
	{
		// The server has no such type, so there is no read to wait for and no defs to merge. The attributes this
		// model has are entirely what the project declares, and the builder produces them from the plan alone,
		// which makes an empty table here a real answer rather than an unread one.
		AttributesTable->SetPlaceholder(
			FText::FromString(CrowdyModelEmptyState::AttributeTable(ECrowdyModelLoadState::Loaded)));
		AttributesTable->SetRows(CrowdyModelListItems(
			CrowdyModelLedger::BuildAttributeRows(TArray<TSharedPtr<FStudioPropertyDef>>(), Plan, TypeName)));
	}
	else
	{
		// Asked of the controller, which is the only place that can tell a model nobody has opened from one whose
		// read is out and from one whose read came back an error. The three used to be one empty table.
		AttributesState = Controller->GetAttributeLoadState(TypeName);
		AttributesTable->SetPlaceholder(FText::FromString(CrowdyModelEmptyState::AttributeTable(AttributesState)));

		const TArray<TSharedPtr<FStudioPropertyDef>>* Defs = Controller->GetPropertyDefsForType(TypeName);
		AttributesTable->SetRows(Defs != nullptr
			? CrowdyModelListItems(CrowdyModelLedger::BuildAttributeRows(*Defs, Plan, TypeName))
			: TArray<TSharedPtr<FCrowdyModelRow>>());
	}

	// Functions and automations were read for the whole app, so a model's own are a filter over what is already in
	// hand. Nothing here goes to the server. The app-wide function list is the one to filter: the other one follows
	// whatever container type was asked for last, which may well be another model entirely.
	const ECrowdyModelLoadState FunctionsState = Controller->GetFamilyLoadState(ECrowdyModelFamily::Functions);
	const ECrowdyModelLoadState AutomationsState = Controller->GetFamilyLoadState(ECrowdyModelFamily::Automations);

	// An empty table on the app entry is not a sentence about a model: the pane's own subtitle says it is attached
	// to none, and the two must not contradict each other.
	FunctionsTable->SetPlaceholder(FText::FromString(bAppWide
		? CrowdyModelEmptyState::FunctionTableAppWide(FunctionsState)
		: CrowdyModelEmptyState::FunctionTable(FunctionsState)));
	FunctionsTable->SetRows(CrowdyModelListItems(
		CrowdyModelLedger::BuildFunctionRows(Controller->GetUnfilteredFunctions(), TypeName, Plan)));

	AutomationsTable->SetPlaceholder(FText::FromString(bAppWide
		? CrowdyModelEmptyState::AutomationTableAppWide(AutomationsState)
		: CrowdyModelEmptyState::AutomationTable(AutomationsState)));
	AutomationsTable->SetRows(CrowdyModelListItems(
		CrowdyModelLedger::BuildAutomationRows(Controller->GetAutomations(), Controller->GetAutomationTriggers(), TypeName, Plan)));

	UpdateTriggerNote();
	UpdateHeader(AttributesState, FunctionsState, AutomationsState);

	// A table carries its highlight over to the row naming the same entity when it can, and drops it when it
	// cannot. Either way the row behind the control has just been rebuilt, so what the control says about it is
	// worked out again from the table rather than left as it was.
	UpdateRowDeleteControl();
	UpdateCrossLinks();
}

void SCrowdyModelDetailPanel::UpdateTriggerNote()
{
	if (!TriggerNoteText.IsValid())
	{
		return;
	}

	FString Note;
	if (Controller.IsValid() && ActiveSection == TEXT("automations"))
	{
		// Only the one pair produces a sentence: automations that landed whose triggers did not. Shown on the
		// section it is about, because "automations are shown" reads as a non sequitur over a table of attributes.
		Note = CrowdyModelEmptyState::AutomationTriggerNote(
			Controller->GetFamilyLoadState(ECrowdyModelFamily::Automations),
			Controller->GetFamilyLoadState(ECrowdyModelFamily::AutomationTriggers));
	}

	TriggerNoteText->SetText(FText::FromString(Note));
	TriggerNoteText->SetVisibility(Note.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
}

bool SCrowdyModelDetailPanel::ShowSectionRow(const FString& SectionKey, const FCrowdyModelRow& Entity)
{
	SetActiveSection(SectionKey);

	const TSharedPtr<SCrowdyModelSectionTable> Table = ActiveTable();
	if (!Table.IsValid() || !Table->SelectRowForEntity(Entity))
	{
		return false;
	}

	// The highlight moved without anyone clicking, so everything that answers for the highlighted row is asked
	// again rather than left describing whichever row was there before.
	UpdateRowDeleteControl();
	UpdateCrossLinks();
	return true;
}

void SCrowdyModelDetailPanel::RefreshRowDeleteControl()
{
	UpdateRowDeleteControl();
}

void SCrowdyModelDetailPanel::OnSectionSelectionChanged(TSharedPtr<FCrowdyModelRow> Row, ESelectInfo::Type SelectInfo)
{
	// Both kinds of change matter here, the reader's and the table's own. A row set replaced under an armed
	// control is announced as a direct change, and it is exactly the case where the control has to disarm.
	UpdateRowDeleteControl();
	UpdateCrossLinks();
}

void SCrowdyModelDetailPanel::UpdateCrossLinks()
{
	CrossLinks.Reset();

	// Asked of the table itself, every time. Replacing a table's rows builds a whole new set of row objects and
	// drops the highlight, so a list computed for a remembered row would describe one that is no longer there.
	const TSharedPtr<SCrowdyModelSectionTable> Table = ActiveTable();
	const TSharedPtr<FCrowdyModelRow> Row =
		(bHasModel && Table.IsValid() && Table->NumSelectedRows() > 0) ? Table->GetSelectedRow() : nullptr;

	FString Summary;

	if (Row.IsValid() && Controller.IsValid())
	{
		// The names this project authors for its attributes, so an attribute link reads the same spelling the grid
		// beside it shows rather than the raw lowercase server key. Empty when no plan has run yet for this app, in
		// which case every attribute link falls back to the key reconstructed into words.
		TMap<FString, FString> AttributeDisplayNames;
		if (const TSharedPtr<const FCrowdyModelSnapshot> Snapshot = Controller->GetModelSnapshot())
		{
			AttributeDisplayNames = Snapshot->AttributeAuthoredNames();
		}

		// The app-wide function list, never the mirror that follows whatever model a view asked for last: a
		// narrowed list would report every other model as having no functions, which is this answer's whole point.
		CrossLinks = CrowdyModelCrossLinks::ForRow(
			*Row,
			Controller->GetUnfilteredFunctions(),
			Controller->GetAutomations(),
			Controller->GetAutomationTriggers(),
			AttributeDisplayNames);

		// The real read states, never Loaded as a placeholder: a zero over a list nobody read is not a zero, and
		// this is the one line that can say which of the two it is. The event triggers are passed on their own
		// terms because they are a separate read that can fail while the automations are present, and every line
		// about an automation waiting on something is computed from it alone.
		Summary = CrowdyModelCrossLinks::SummaryLine(
			CrossLinks.Num(),
			Controller->GetFamilyLoadState(ECrowdyModelFamily::Functions),
			Controller->GetFamilyLoadState(ECrowdyModelFamily::Automations),
			Controller->GetFamilyLoadState(ECrowdyModelFamily::AutomationTriggers));

		// The disclosure's open state belongs to the entity it describes. A read landing repaints these lines for
		// the same row, and closing it there would be a repaint undoing what the reader just did.
		const FString Key = EntityKey(*Row);
		if (!Key.Equals(CrossLinkEntityKey, ESearchCase::CaseSensitive))
		{
			CrossLinkEntityKey = Key;
			bCrossLinksExpanded = false;
		}
	}
	else
	{
		CrossLinkEntityKey.Reset();
		bCrossLinksExpanded = false;
	}

	if (CrossLinkSummaryText.IsValid())
	{
		CrossLinkSummaryText->SetText(FText::FromString(Summary));
		CrossLinkSummaryText->SetVisibility(Summary.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}

	if (CrossLinkToggleBox.IsValid())
	{
		CrossLinkToggleBox->SetVisibility(CrossLinks.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed);
	}
	if (CrossLinkToggleLabel.IsValid())
	{
		CrossLinkToggleLabel->SetText(bCrossLinksExpanded
			? FText::Format(LOCTEXT("HideCrossLinks", "Hide {0}"), FText::AsNumber(CrossLinks.Num()))
			: FText::Format(LOCTEXT("ShowCrossLinks", "Show {0}"), FText::AsNumber(CrossLinks.Num())));
	}

	if (!CrossLinkLinesBox.IsValid())
	{
		return;
	}

	CrossLinkLinesBox->ClearChildren();
	CrossLinkLinesBox->SetVisibility(
		(bCrossLinksExpanded && CrossLinks.Num() > 0) ? EVisibility::Visible : EVisibility::Collapsed);

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	for (const FCrowdyModelLink& Link : CrossLinks)
	{
		if (!Link.bNavigable || !ShowLink.IsBound())
		{
			// Shown and not clickable. Reporting nothing would hide a real dependency and guessing a model would
			// send the reader somewhere wrong; the reason it cannot be followed is already inside the line.
			CrossLinkLinesBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.TextStyle(&Style, "Crowdy.Text.Subtle")
				.AutoWrapText(true)
				.Text(FText::FromString(Link.Display))
			];
			continue;
		}

		CrossLinkLinesBox->AddSlot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f).HAlign(HAlign_Left)
		[
			SNew(SButton)
			.ButtonStyle(&Style, "Crowdy.Button.Ghost")
			.ContentPadding(FMargin(4.0f, 2.0f))
			.ToolTipText(FText::FromString(Link.Display))
			// The link travels by value: the four fields that name the target, built when the line was built and
			// re-resolved on the far side against whatever its table holds at that instant.
			.OnClicked(this, &SCrowdyModelDetailPanel::OnFollowCrossLink, Link)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Link.Display))
				.TextStyle(&Style, "Crowdy.Text.Body")
				.AutoWrapText(true)
			]
		];
	}
}

FReply SCrowdyModelDetailPanel::OnToggleCrossLinks()
{
	bCrossLinksExpanded = !bCrossLinksExpanded;
	UpdateCrossLinks();
	return FReply::Handled();
}

FReply SCrowdyModelDetailPanel::OnFollowCrossLink(FCrowdyModelLink Link)
{
	if (!Link.bNavigable)
	{
		return FReply::Handled();
	}

	// The entity to land on, named rather than pointed at. IsSameEntity compares exactly these three fields, so
	// this is the same rule a table uses to carry a highlight across a rebuild.
	FCrowdyModelRow Entity;
	Entity.Kind = Link.Kind;
	Entity.Name = Link.TargetName;
	Entity.OwningType = Link.TargetModel;

	ShowLink.ExecuteIfBound(Link.TargetModel, Link.TargetSection, Entity);
	return FReply::Handled();
}

void SCrowdyModelDetailPanel::UpdateRowDeleteControl()
{
	bRowMarkEnabled = false;
	RowMarkLabel = LOCTEXT("MarkRow", "Mark for deletion");
	RowMarkTooltip = LOCTEXT("MarkRowNoSelection", "Highlight a row to mark it for deletion.");

	FText GateReason;

	const TSharedPtr<SCrowdyModelSectionTable> Table = ActiveTable();

	// Asked of the table itself, every time. Replacing a table's rows builds a whole new set of row objects and
	// drops the highlight, so a control that remembered one would stay armed at a row that is no longer there.
	if (bHasModel && Table.IsValid() && Table->NumSelectedRows() > 0 && ToggleRowMark.IsBound())
	{
		if (const TSharedPtr<FCrowdyModelRow> Row = Table->GetSelectedRow())
		{
			const FCrowdyDeleteGate Gate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(*Row);
			if (Gate.bAllowed)
			{
				const bool bAlreadyMarked = IsRowMarked.IsBound() && IsRowMarked.Execute(*Row);
				bRowMarkEnabled = true;
				RowMarkLabel = bAlreadyMarked ? LOCTEXT("UnmarkRow", "Unmark") : LOCTEXT("MarkRow", "Mark for deletion");
				RowMarkTooltip = bAlreadyMarked
					? LOCTEXT("UnmarkRowTip", "Take this row out of the marked set. Nothing has been deleted.")
					: LOCTEXT("MarkRowTip", "Add this row to the marked set. Nothing is read or deleted until the review is opened.");
			}
			else
			{
				RowMarkTooltip = FText::FromString(Gate.Reason);
				GateReason = FText::FromString(Gate.Reason);
			}
		}
	}

	if (MarkRowLabelText.IsValid())
	{
		MarkRowLabelText->SetText(RowMarkLabel);
	}

	if (RowGateReasonText.IsValid())
	{
		RowGateReasonText->SetText(GateReason);
		RowGateReasonText->SetVisibility(GateReason.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}
}

FReply SCrowdyModelDetailPanel::OnMarkRowClicked()
{
	// The target is read from the table at this instant, not from anything remembered when the control was drawn.
	// An app-identity check cannot catch a control acting on a row that is no longer the one on screen, because
	// what has gone wrong there is target identity.
	const TSharedPtr<SCrowdyModelSectionTable> Table = ActiveTable();
	if (!Table.IsValid() || Table->NumSelectedRows() == 0)
	{
		return FReply::Handled();
	}

	const TSharedPtr<FCrowdyModelRow> Row = Table->GetSelectedRow();
	if (!Row.IsValid())
	{
		return FReply::Handled();
	}

	// The same gate the control was drawn from, asked again: what a row is allowed to offer can change between
	// the moment the control was drawn and the moment it was pressed.
	if (!CrowdyGameModelDelete::CanDeleteFromPrimaryView(*Row).bAllowed)
	{
		UpdateRowDeleteControl();
		return FReply::Handled();
	}

	ToggleRowMark.ExecuteIfBound(*Row);
	UpdateRowDeleteControl();
	return FReply::Handled();
}

void SCrowdyModelDetailPanel::UpdateHeader(ECrowdyModelLoadState AttributesState,
	ECrowdyModelLoadState FunctionsState, ECrowdyModelLoadState AutomationsState)
{
	if (!bHasModel)
	{
		return;
	}

	// Built here, on the one event that can change any of it, rather than in a text binding. A bound lambda runs
	// on every frame the pane is on screen, and none of this changes between frames.
	TitleText->SetText(FText::FromString(Model.Display));

	// Whether this model leads anywhere, worked out once here rather than in the button's own enabled binding: a
	// binding runs every frame the pane is on screen, and this answer can only move when the model does.
	bCanOpenModel = CrowdyModelCodeLink::CanOpen(Model.CodePath);

	const bool bAppWide = Model.TypeName.IsEmpty();

	// The type name, then where this model came from and how it differs, each only when there is something to say.
	// A model that is on the server and matches the project adds nothing here, exactly as its row's Source and
	// Status cells stay empty: the page spends words in proportion to what needs attention.
	FString Subtitle = Model.TypeName;
	const bool bSynced = Model.Provenance == ECrowdyModelProvenance::CodeSynced;
	for (const FString& Note : { bSynced ? FString() : Model.ProvenanceText, Model.DriftText })
	{
		if (!Note.IsEmpty())
		{
			Subtitle += Subtitle.IsEmpty() ? Note : TEXT("   ") + Note;
		}
	}

	SubtitleText->SetText(bAppWide
		? LOCTEXT("AppWideSubtitle", "Not attached to any model")
		: FText::FromString(Subtitle));

	const FString Description = Model.Description.TrimStartAndEnd();
	DescriptionText->SetText(FText::FromString(Description));
	DescriptionText->SetVisibility(Description.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);

	// Every part gets a count only when its own read landed, and otherwise a phrase for the state that read is in.
	// All three are needed: a header reading "0 functions" a few lines above a table saying the functions could not
	// be read puts two contradictory answers about one list on the same pane.
	const FString Functions = CountPhrase(
		FunctionsState, FunctionsTable->NumRows(), TEXT("Functions"), TEXT("function"), TEXT("functions"));
	const FString Automations = CountPhrase(
		AutomationsState, AutomationsTable->NumRows(), TEXT("Automations"), TEXT("automation"), TEXT("automations"));

	FString Counts;
	if (bAppWide)
	{
		Counts = FString::Printf(TEXT("%s, %s"), *Functions, *Automations);
	}
	else
	{
		const FString Attributes = CountPhrase(
			AttributesState, AttributesTable->NumRows(), TEXT("Attributes"), TEXT("attribute"), TEXT("attributes"));
		Counts = FString::Printf(TEXT("%s, %s, %s"), *Attributes, *Functions, *Automations);
	}

	CountsText->SetText(FText::FromString(Counts));
}

TSharedPtr<SCrowdyModelSectionTable> SCrowdyModelDetailPanel::ActiveTable() const
{
	switch (SectionIndex(ActiveSection))
	{
	case 1:  return FunctionsTable;
	case 2:  return AutomationsTable;
	default: return AttributesTable;
	}
}

bool SCrowdyModelDetailPanel::IsOpenRowEnabled()
{
	const TSharedPtr<SCrowdyModelSectionTable> Table = ActiveTable();

	// Asked of the table itself every time. Replacing a table's rows builds a whole new set of row objects and drops
	// the highlight, so a control that remembered a row would stay armed at one that is no longer on screen.
	if (!Table.IsValid() || Table->NumSelectedRows() == 0)
	{
		return false;
	}

	const TSharedPtr<FCrowdyModelRow> Row = Table->GetSelectedRow();
	const FString Path = Row.IsValid() ? Row->CodePath : FString();

	if (!Path.Equals(CheckedRowPath, ESearchCase::CaseSensitive))
	{
		CheckedRowPath = Path;
		bCheckedRowPathOpens = CrowdyModelCodeLink::CanOpen(Path);
	}
	return bCheckedRowPathOpens;
}

FReply SCrowdyModelDetailPanel::OnOpenModelClicked()
{
	CrowdyModelCodeLink::Open(Model.CodePath);
	return FReply::Handled();
}

FReply SCrowdyModelDetailPanel::OnOpenRowClicked()
{
	// The target is read from the table at this instant, not from anything remembered when the control was drawn.
	const TSharedPtr<SCrowdyModelSectionTable> Table = ActiveTable();
	if (!Table.IsValid() || Table->NumSelectedRows() == 0)
	{
		return FReply::Handled();
	}

	if (const TSharedPtr<FCrowdyModelRow> Row = Table->GetSelectedRow())
	{
		CrowdyModelCodeLink::Open(Row->CodePath);
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
