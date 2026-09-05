// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyModelBrowserTab.h"

#include "GameModel/CrowdyGameModelDelete.h"
#include "GameModel/CrowdyModelLoadState.h"
#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "Styling/SlateTypes.h"
#include "Styling/StyleDefaults.h"
#include "Types/WidgetActiveTimerDelegate.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/GameModel/SCrowdyDeleteReviewPanel.h"
#include "UI/GameModel/SCrowdyModelDetailPanel.h"
#include "UI/GameModel/SCrowdyModelSectionTable.h"
#include "UI/GameModel/SCrowdyProvenanceGlyph.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// The keys the source strip stores. They are never shown; the labels beside them are.
	const TCHAR* const CrowdyModelSourceKeyAll = TEXT("all");
	const TCHAR* const CrowdyModelSourceKeyInCode = TEXT("in-code");
	const TCHAR* const CrowdyModelSourceKeyServerOnly = TEXT("server-only");
}

FString SCrowdyModelBrowserTab::CountPhrase(int32 Count, const TCHAR* Singular, const TCHAR* Plural)
{
	return FString::Printf(TEXT("%d %s"), Count, Count == 1 ? Singular : Plural);
}

ECrowdyModelSourceFilter SCrowdyModelBrowserTab::FilterForKey(const FString& Key)
{
	if (Key == CrowdyModelSourceKeyInCode)
	{
		return ECrowdyModelSourceFilter::InCode;
	}
	if (Key == CrowdyModelSourceKeyServerOnly)
	{
		return ECrowdyModelSourceFilter::OnlyOnServer;
	}
	return ECrowdyModelSourceFilter::All;
}

void SCrowdyModelBrowserTab::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyModelBrowserTab::HandleAppChanged);
		Controller->OnContainerTypesChanged.AddSP(this, &SCrowdyModelBrowserTab::HandleContainerTypesChanged);
		Controller->OnFunctionsChanged.AddSP(this, &SCrowdyModelBrowserTab::HandleFunctionsChanged);
		Controller->OnAutomationsChanged.AddSP(this, &SCrowdyModelBrowserTab::HandleAutomationsChanged);
		// The cache signal rather than the mirror one: this tab reads a model's attributes by type, and the mirror
		// belongs to whichever view asked for a type last, which may well be another tab.
		Controller->OnPropertyDefsCached.AddSP(this, &SCrowdyModelBrowserTab::HandlePropertyDefsChanged);
		// The snapshot signal, not the report one: the report is also cleared at the start of every fresh plan, and
		// following that would blank the source and status of every row for as long as the plan takes to run.
		Controller->OnModelSnapshotChanged.AddSP(this, &SCrowdyModelBrowserTab::HandleModelSnapshotChanged);
	}

	SourceFilterKey = CrowdyModelSourceKeyAll;

	// The app this tab's contents belong to, as of construction. The page is built long after a remembered app was
	// restored and selected, so without this the first list to arrive reads as an app switch and empties the search
	// box the user has just typed into.
	EditorAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> ModelRail =
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SAssignNew(SearchBox, SSearchBox)
			.Style(&Style, "Crowdy.SearchBox")
			.HintText(LOCTEXT("SearchModels", "Search models"))
			// The search box holds its own notifications back until typing pauses, so a fast typist filters the
			// list once rather than once per key.
			.DelayChangeNotificationsWhileTyping(true)
			.OnTextChanged(this, &SCrowdyModelBrowserTab::OnSearchTextChanged)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			// The neutral strip, not the segmented control: that one marks its choice in the brand gold, and this
			// page spends its colour on nothing at all.
			CrowdyStudioWidgets::TabStrip(
				{ CrowdyModelSourceKeyAll, CrowdyModelSourceKeyInCode, CrowdyModelSourceKeyServerOnly },
				{ LOCTEXT("SourceAll", "All"), LOCTEXT("SourceInCode", "In code"), LOCTEXT("SourceServerOnly", "Only on server") },
				TAttribute<FString>::CreateSP(this, &SCrowdyModelBrowserTab::GetSourceFilterKey),
				[this](const FString& Key) { OnSourceFilterSelected(Key); })
		]

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			CrowdyStudioWidgets::Card(
				SNew(SOverlay)

				+ SOverlay::Slot()
				[
					SAssignNew(ModelListView, SListView<TSharedPtr<FCrowdyModelSummary>>)
					.ListViewStyle(&Style, "Crowdy.TableView")
					.ListItemsSource(&FilteredModels)
					.OnGenerateRow(this, &SCrowdyModelBrowserTab::MakeModelRow)
					.OnSelectionChanged(this, &SCrowdyModelBrowserTab::OnModelSelectionChanged)
					.SelectionMode(ESelectionMode::Single)
				]

				+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SNew(SBox)
					.Padding(18.0f)
					// Hit-test invisible so the message never swallows a scroll aimed at the list underneath it.
					.Visibility_Lambda([this]() { return FilteredModels.Num() == 0 ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 10.0f)
						[ CrowdyStudioWidgets::Icon(TEXT("cube"), 26.0f, FSlateColor(FCrowdyStudioStyle::TextSubtle())) ]
						+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
						[
							SAssignNew(ModelListPlaceholderText, STextBlock)
							.TextStyle(&Style, "Crowdy.Text.Subtle")
							.Justification(ETextJustify::Center)
							.AutoWrapText(true)
						]
					]
				],
				FMargin(4.0f), /*bFlat*/ true)
		]

		// Marking a model, and the way into the review over everything marked. Marking reads nothing: what a set
		// of marks would do is worked out once, when the review is opened.
		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 8.0f, 2.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(10.0f, 4.0f))
				.ToolTipText_Lambda([this]() { return ModelMarkTooltip; })
				.IsEnabled_Lambda([this]() { return bModelMarkEnabled; })
				.OnClicked(this, &SCrowdyModelBrowserTab::OnMarkModelClicked)
				[
					SAssignNew(MarkModelLabelText, STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SSpacer)
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SAssignNew(ReviewButtonBox, SBox)
				.Visibility(EVisibility::Collapsed)
				[
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Secondary")
					.ContentPadding(FMargin(10.0f, 4.0f))
					.ToolTipText(LOCTEXT("ReviewDeletionsTip",
						"Check what the marked set would do before any of it runs. This is the only point at which anything is read."))
					.OnClicked(this, &SCrowdyModelBrowserTab::OnReviewDeletionsClicked)
					[
						SAssignNew(ReviewButtonLabelText, STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
		]

		// Why the highlighted model offers no delete, said where the control is rather than in a dialog after the
		// click. The pane on the right carries the Open that goes to whatever declares it.
		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 5.0f, 2.0f, 0.0f)
		[
			SAssignNew(ModelGateReasonText, STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
			.Visibility(EVisibility::Collapsed)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 6.0f, 2.0f, 0.0f)
		[
			// How old the source and status columns are. A table full of confident-looking cells does not go grey
			// the way a stale paragraph reads as stale, so it says here, once and quietly, what moment it describes.
			SAssignNew(FreshnessText, STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
		];

	ChildSlot
	[
		SNew(SSplitter)
		.Style(&Style, "Crowdy.Splitter")
		.Orientation(Orient_Horizontal)
		.PhysicalSplitterHandleSize(3.0f)

		+ SSplitter::Slot().Value(0.33f).MinSize(200.0f)
		[
			SNew(SBox).Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[ ModelRail ]
		]

		+ SSplitter::Slot().Value(0.67f).MinSize(300.0f)
		[
			SNew(SBox).Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SAssignNew(DetailPanel, SCrowdyModelDetailPanel)
				.Controller(Controller)
				.IsRowMarked(this, &SCrowdyModelBrowserTab::IsRowMarkedForDelete)
				.OnToggleRowMark(this, &SCrowdyModelBrowserTab::ToggleRowDeleteMark)
				.OnShowLink(this, &SCrowdyModelBrowserTab::HandleShowCrossLink)
			]
		]

		// The review, beside what it is about. Collapsed while nothing is marked, so a page nobody is deleting
		// from keeps its whole width for the list and the model on it. A splitter leaves a collapsed slot out of
		// its arrangement entirely, handle included.
		+ SSplitter::Slot().Value(0.34f).MinSize(260.0f)
		[
			SAssignNew(DeletePanelBox, SBox)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			.Visibility(EVisibility::Collapsed)
			[
				SAssignNew(DeletePanel, SCrowdyDeleteReviewPanel)
				.Controller(Controller)
				.OnMarksChanged(this, &SCrowdyModelBrowserTab::HandleMarksChanged)
				.OnCommitFinished(this, &SCrowdyModelBrowserTab::HandleDeleteCommitFinished)
				.OnShowModel(this, &SCrowdyModelBrowserTab::HandleShowBlockingModel)
			]
		]
	];

	RebuildLedger();
	UpdateModelDeleteControl();
}

SCrowdyModelBrowserTab::~SCrowdyModelBrowserTab()
{
	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.RemoveAll(this);
		Controller->OnContainerTypesChanged.RemoveAll(this);
		Controller->OnFunctionsChanged.RemoveAll(this);
		Controller->OnAutomationsChanged.RemoveAll(this);
		Controller->OnPropertyDefsCached.RemoveAll(this);
		Controller->OnModelSnapshotChanged.RemoveAll(this);
	}
}

void SCrowdyModelBrowserTab::SelectModel(const FString& TypeName)
{
	// A list arriving only marks the model list dirty; the rebuild itself runs on an active timer, and an active
	// timer runs while its widget is painted. A tab sitting in the inactive half of a switcher is never painted,
	// so this call arrives with the rebuild still owed and would search a list from before the app was read. Any
	// caller reaching in from outside the paint path has to settle that first.
	FlushPendingRefresh();

	const bool bListed = AllModels.ContainsByPredicate(
		[&TypeName](const FCrowdyModelSummary& Candidate)
		{
			// Type names are server keys and the default string comparison is case-insensitive.
			return Candidate.TypeName.Equals(TypeName, ESearchCase::CaseSensitive);
		});

	if (!bListed)
	{
		// Pruned from the app between reads. Whatever is open here is still valid, so it stays open.
		return;
	}

	// A search that hides the row would leave nothing for the list to select or scroll to; clear it exactly as
	// the user would, so the row this call is about to open is actually on screen.
	if (!SearchQuery.IsEmpty())
	{
		SearchQuery.Reset();
		if (SearchBox.IsValid())
		{
			SearchBox->SetText(FText::GetEmpty());
		}
		ApplyFilter(/*bModelsRebuilt*/ false);
	}

	SelectedTypeName = TypeName;
	RestoreSelection(/*bModelsRebuilt*/ false);
	UpdateModelDeleteControl();

	if (!ScrollTimerHandle.IsValid())
	{
		ScrollTimerHandle = RegisterActiveTimer(0.0f,
			FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyModelBrowserTab::HandleScrollToSelection));
	}
}

void SCrowdyModelBrowserTab::SelectModelRow(const FString& TypeName, const FString& SectionKey, const FCrowdyModelRow& Entity)
{
	// Whatever this navigation was waiting for, it is superseded by this one.
	bHasPendingRow = false;

	SelectModel(TypeName);

	if (SectionKey.IsEmpty())
	{
		return;
	}

	// SelectModel is a no-op for a model this browser does not list, and leaves whatever was open still open.
	// Pointing a section at a row of a model nobody opened would highlight a row on the wrong model entirely.
	if (!SelectedTypeName.IsSet() || !SelectedTypeName.GetValue().Equals(TypeName, ESearchCase::CaseSensitive))
	{
		return;
	}

	if (DetailPanel.IsValid() && DetailPanel->ShowSectionRow(SectionKey, Entity))
	{
		return;
	}

	// No such row yet. The one case that reaches here is a model whose attributes are read when it is first
	// opened: SelectModel has just issued that read, and the highlight is completed from its reply. The
	// navigation itself issues nothing SelectModel would not already have issued.
	PendingRowModel = TypeName;
	PendingRowSection = SectionKey;
	PendingRowEntity = Entity;
	bHasPendingRow = true;
}

void SCrowdyModelBrowserTab::TryCompletePendingRow()
{
	if (!bHasPendingRow || !DetailPanel.IsValid())
	{
		return;
	}

	// The reader opened something else while the read was out. Their choice outranks a navigation they have
	// already moved on from.
	if (!SelectedTypeName.IsSet() || !SelectedTypeName.GetValue().Equals(PendingRowModel, ESearchCase::CaseSensitive))
	{
		bHasPendingRow = false;
		return;
	}

	if (DetailPanel->ShowSectionRow(PendingRowSection, PendingRowEntity))
	{
		bHasPendingRow = false;
		return;
	}

	// Still not there. Worth waiting for only while a read is actually out: a read that failed leaves the
	// placeholder saying so and nothing more is coming, and an entity the read simply does not carry is not going
	// to appear either.
	if (!Controller.IsValid()
		|| Controller->GetAttributeLoadState(PendingRowModel) != ECrowdyModelLoadState::Loading)
	{
		bHasPendingRow = false;
	}
}

EActiveTimerReturnType SCrowdyModelBrowserTab::HandleScrollToSelection(double InCurrentTime, float InDeltaTime)
{
	ScrollTimerHandle.Reset();

	if (SelectedTypeName.IsSet() && ModelListView.IsValid())
	{
		const FString& OpenType = SelectedTypeName.GetValue();
		for (const TSharedPtr<FCrowdyModelSummary>& Item : FilteredModels)
		{
			if (Item.IsValid() && Item->TypeName.Equals(OpenType, ESearchCase::CaseSensitive))
			{
				ModelListView->RequestScrollIntoView(Item);
				break;
			}
		}
	}

	return EActiveTimerReturnType::Stop;
}

void SCrowdyModelBrowserTab::HandleAppChanged()
{
	SyncEditorAppScope();
	MarkLedgerDirty();
}

void SCrowdyModelBrowserTab::HandleContainerTypesChanged()
{
	// An app switch empties this list before anything else happens, so this is the earliest and the most reliable
	// point at which the switch is visible to this tab.
	SyncEditorAppScope();
	MarkLedgerDirty();
}

void SCrowdyModelBrowserTab::HandleFunctionsChanged()
{
	MarkLedgerDirty();
}

void SCrowdyModelBrowserTab::HandleAutomationsChanged()
{
	MarkLedgerDirty();
}

void SCrowdyModelBrowserTab::HandlePropertyDefsChanged()
{
	// Attributes belong to the open model alone. The model list and its counts do not move when they land, so
	// only the pane on the right is repainted.
	MarkDetailDirty();
}

void SCrowdyModelBrowserTab::HandleModelSnapshotChanged()
{
	// Every row's source and status, every synthesized row, and every count that includes one, all come from the
	// plan. There is no narrower pass than rebuilding them.
	MarkLedgerDirty();
}

void SCrowdyModelBrowserTab::SyncEditorAppScope()
{
	const int64 CurrentAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;
	if (CurrentAppId == EditorAppId)
	{
		return;
	}

	EditorAppId = CurrentAppId;
	ClearAppScopedEditors();
}

void SCrowdyModelBrowserTab::ClearAppScopedEditors()
{
	// Everything on this tab names entities of the app it was read for: the search text, the source setting, the
	// open model, and the three tables under it. A type name carried over from the previous app is read as one of
	// the new app's, so none of it survives the switch. The source setting goes with them because it is a claim
	// about the previous app's plan, and the new app may have no plan at all.
	SearchQuery.Reset();
	if (SearchBox.IsValid())
	{
		SearchBox->SetText(FText::GetEmpty());
	}
	SourceFilterKey = CrowdyModelSourceKeyAll;

	SelectedTypeName.Reset();
	AllModels.Reset();
	FilteredModels.Reset();

	if (ModelListView.IsValid())
	{
		ModelListView->ClearSelection();
		ModelListView->RequestListRefresh();
	}
	ClearDetailPanel();

	UpdateListPlaceholder();
	UpdateFreshnessLine();
	// The review panel empties its own marks on the same switch, for the same reason: a mark names an entity in
	// one app and means nothing in another.
	UpdateModelDeleteControl();
}

void SCrowdyModelBrowserTab::MarkLedgerDirty()
{
	bLedgerDirty = true;
	ScheduleRefresh();
}

void SCrowdyModelBrowserTab::MarkFilterDirty()
{
	bFilterDirty = true;
	ScheduleRefresh();
}

void SCrowdyModelBrowserTab::MarkDetailDirty()
{
	bDetailDirty = true;
	ScheduleRefresh();
}

void SCrowdyModelBrowserTab::ScheduleRefresh()
{
	if (RefreshTimerHandle.IsValid())
	{
		return;
	}

	RefreshTimerHandle = RegisterActiveTimer(0.0f,
		FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyModelBrowserTab::HandleScheduledRefresh));
}

EActiveTimerReturnType SCrowdyModelBrowserTab::HandleScheduledRefresh(double InCurrentTime, float InDeltaTime)
{
	RefreshTimerHandle.Reset();
	FlushPendingRefresh();
	return EActiveTimerReturnType::Stop;
}

void SCrowdyModelBrowserTab::FlushPendingRefresh()
{
	if (bLedgerDirty)
	{
		// A rebuild filters and repaints the pane on its way through, so the two narrower passes have nothing
		// left to do.
		bLedgerDirty = false;
		bFilterDirty = false;
		bDetailDirty = false;
		RebuildLedger();
		TryCompletePendingRow();
		return;
	}

	if (bFilterDirty)
	{
		bFilterDirty = false;
		ApplyFilter(/*bModelsRebuilt*/ false);
	}

	if (bDetailDirty)
	{
		bDetailDirty = false;
		if (DetailPanel.IsValid())
		{
			DetailPanel->RefreshSections();
		}
	}

	// The rows a waiting navigation needs are built by the passes above, so this is the first moment the target
	// can be there at all.
	TryCompletePendingRow();
}

void SCrowdyModelBrowserTab::RebuildLedger()
{
	AllModels.Reset();

	if (Controller.IsValid())
	{
		// The app-wide function list, not the one that follows whatever container type was asked for last: this tab
		// shows every model at once, and a list narrowed to one of them would report all the others as having none.
		const TArray<TSharedPtr<FStudioFunction>>& AppFunctions = Controller->GetUnfilteredFunctions();

		// The last finished plan for this app, or null when nobody has run one. With it, each model is classified
		// against the project and a model the project declares that the server has never seen becomes a row of its
		// own; without it every model reads as unclassified, which is what a page nobody has checked must say.
		const TSharedPtr<const FCrowdyModelSnapshot> Snapshot = Controller->GetModelSnapshot();

		AllModels = CrowdyModelLedger::BuildModelList(
			Controller->GetContainerTypes(), AppFunctions, Controller->GetAutomations(), Snapshot.Get());

		// A function or an automation can name no model at all, in which case it belongs to the app rather than to
		// any one model and no model's page would ever show it. Those get an entry of their own, whose empty type
		// name is exactly what addresses them. The counts come from building their rows because that is the same
		// answer the entry shows when it is opened, so the two can never disagree.
		FCrowdyModelSummary AppWide;
		AppWide.Display = TEXT("App-wide");
		AppWide.Description = TEXT("Functions and automations that are not attached to a model.");
		AppWide.FunctionCount = CrowdyModelLedger::BuildFunctionRows(AppFunctions, FString(), Snapshot.Get()).Num();
		AppWide.AutomationCount = CrowdyModelLedger::BuildAutomationRows(
			Controller->GetAutomations(), Controller->GetAutomationTriggers(), FString(), Snapshot.Get()).Num();
		AppWide.AttributeCount = 0;
		AppWide.SearchKey = CrowdyModelLedger::MakeSearchKey({ AppWide.Display, AppWide.Description });

		if (AppWide.FunctionCount > 0 || AppWide.AutomationCount > 0)
		{
			AllModels.Insert(MoveTemp(AppWide), 0);
		}
	}

	UpdateFreshnessLine();
	ApplyFilter(/*bModelsRebuilt*/ true);
}

void SCrowdyModelBrowserTab::ApplyFilter(bool bModelsRebuilt)
{
	// The query is lowercased once and compared against each model's prebuilt key, so no row rebuilds its haystack
	// and no text is folded per keystroke. The source setting narrows the same pass rather than a second one, so the
	// two compose and neither can widen what the other allows. The matches themselves are copied into the list
	// view's own entries, which is what a list view holds its items by.
	FilteredModels = CrowdyModelListItems(
		CrowdyModelLedger::FilterModels(AllModels, SearchQuery, FilterForKey(SourceFilterKey)));

	if (ModelListView.IsValid())
	{
		ModelListView->RequestListRefresh();
	}

	UpdateListPlaceholder();
	RestoreSelection(bModelsRebuilt);

	// The list's row objects were all replaced, so the control acting on the highlighted one is now pointed at a
	// new object or at nothing at all. Either way its word is worked out again rather than left as it was: a
	// destructive control still armed at a row that has gone is the shape this page has been bitten by before.
	UpdateModelDeleteControl();
}

void SCrowdyModelBrowserTab::ShowInDetailPanel(const FCrowdyModelSummary& Summary)
{
	if (DetailPanel.IsValid())
	{
		DetailPanel->SetModel(Summary);
	}
	ShownTypeName = Summary.TypeName;
}

void SCrowdyModelBrowserTab::ClearDetailPanel()
{
	if (DetailPanel.IsValid())
	{
		DetailPanel->ClearModel();
	}
	ShownTypeName.Reset();
}

void SCrowdyModelBrowserTab::RestoreSelection(bool bModelsRebuilt)
{
	if (!DetailPanel.IsValid())
	{
		return;
	}

	if (!SelectedTypeName.IsSet())
	{
		if (ModelListView.IsValid())
		{
			ModelListView->ClearSelection();
		}
		ClearDetailPanel();
		return;
	}

	const FString& OpenType = SelectedTypeName.GetValue();
	const FCrowdyModelSummary* StillPresent = AllModels.FindByPredicate(
		[&OpenType](const FCrowdyModelSummary& Candidate)
		{
			// Type names are server keys and the default string comparison is case-insensitive.
			return Candidate.TypeName.Equals(OpenType, ESearchCase::CaseSensitive);
		});

	if (StillPresent == nullptr)
	{
		// The model that was open is not in this app's list any more. Its sections describe something that is not
		// there, so the pane goes back to showing nothing rather than describing a model that has gone.
		SelectedTypeName.Reset();
		if (ModelListView.IsValid())
		{
			ModelListView->ClearSelection();
		}
		ClearDetailPanel();
		return;
	}

	if (ModelListView.IsValid())
	{
		// Every row object was rebuilt, so the highlight has to be placed on the new row that carries the same
		// name. A search that hides the row leaves the pane on the model it was showing: the model still exists,
		// and a filter is a view over the list rather than a statement about what the app has.
		ModelListView->ClearSelection();
		for (const TSharedPtr<FCrowdyModelSummary>& Item : FilteredModels)
		{
			if (Item.IsValid() && Item->TypeName.Equals(OpenType, ESearchCase::CaseSensitive))
			{
				// Announced as a direct change, which the selection handler ignores by design, so the pane is
				// told what to show explicitly rather than through the list.
				ModelListView->SetSelection(Item, ESelectInfo::Direct);
				break;
			}
		}
	}

	// Only hand the pane a model it is not already showing, unless the summaries themselves were rebuilt and the one
	// it holds is out of date. Pushing the same model again replaces the rows in all three section tables, and a
	// replaced row set drops whichever row the user had highlighted.
	const bool bAlreadyShown = ShownTypeName.IsSet() && ShownTypeName.GetValue().Equals(OpenType, ESearchCase::CaseSensitive);
	if (bModelsRebuilt || !bAlreadyShown)
	{
		ShowInDetailPanel(*StillPresent);
	}
}

void SCrowdyModelBrowserTab::UpdateListPlaceholder()
{
	if (!ModelListPlaceholderText.IsValid())
	{
		return;
	}

	// An app with nothing in it, a read nobody has made, a read still out, a read that failed, a search that
	// matched nothing and a source setting that left nothing are six different situations. A single "nothing here"
	// would leave a reader unable to tell which one they have, and would report a server error as a design fact.
	if (AllModels.Num() == 0)
	{
		ModelListPlaceholderText->SetText(FText::FromString(CrowdyModelEmptyState::ModelRail(
			Controller.IsValid()
				? Controller->GetFamilyLoadState(ECrowdyModelFamily::Models)
				: ECrowdyModelLoadState::NeverRequested)));
	}
	else if (SearchQuery.TrimStartAndEnd().IsEmpty())
	{
		// Named what was filtered rather than a load state: no read can produce this message, and only the control
		// that produced it knows what to say about undoing it.
		ModelListPlaceholderText->SetText(FText::FromString(CrowdyModelEmptyState::ModelRailFilteredBySource()));
	}
	else
	{
		ModelListPlaceholderText->SetText(
			FText::FromString(CrowdyModelEmptyState::ModelRailFilteredBySearch(SearchQuery)));
	}
}

void SCrowdyModelBrowserTab::UpdateFreshnessLine()
{
	if (!FreshnessText.IsValid())
	{
		return;
	}

	TSharedPtr<const FCrowdyModelSnapshot> Snapshot;
	if (Controller.IsValid())
	{
		Snapshot = Controller->GetModelSnapshot();
	}

	if (!Snapshot.IsValid())
	{
		FreshnessText->SetText(LOCTEXT("SchemaNotCheckedYet",
			"Not checked against the project yet. Press Preview changes above."));
		return;
	}

	// An absolute time rather than "a moment ago": this line is only rebuilt when the plan behind it changes, so a
	// relative phrase would freeze at whatever it said when the plan finished and grow quietly wrong.
	const FDateTime LocalCapture = Snapshot->CapturedAt + (FDateTime::Now() - FDateTime::UtcNow());
	const FString Clock = LocalCapture.ToString(TEXT("%H:%M"));

	FreshnessText->SetText(LocalCapture.GetDate() == FDateTime::Now().GetDate()
		? FText::Format(LOCTEXT("SchemaCheckedToday", "Checked against the project at {0}."), FText::FromString(Clock))
		: FText::Format(LOCTEXT("SchemaCheckedEarlier", "Checked against the project on {0} at {1}."),
			FText::FromString(LocalCapture.ToString(TEXT("%Y-%m-%d"))), FText::FromString(Clock)));
}

void SCrowdyModelBrowserTab::OnSourceFilterSelected(const FString& Key)
{
	if (Key == SourceFilterKey)
	{
		return;
	}

	SourceFilterKey = Key;

	// The same single-pass refresh a keystroke takes. A source setting decides which models are LISTED and never
	// what any of them is made of, so it must not push the open model back into the detail pane: that would rebuild
	// all three of its tables and drop whichever row was highlighted.
	MarkFilterDirty();
}

void SCrowdyModelBrowserTab::OnSearchTextChanged(const FText& NewText)
{
	SearchQuery = NewText.ToString();
	MarkFilterDirty();
}

void SCrowdyModelBrowserTab::OnModelSelectionChanged(TSharedPtr<FCrowdyModelSummary> Model, ESelectInfo::Type SelectInfo)
{
	// The list announces the clear it performs when its source is replaced as a direct change. That is not the
	// user choosing anything, and following it would close the open model on every refresh.
	if (SelectInfo == ESelectInfo::Direct)
	{
		return;
	}

	// Read what the list is showing as selected right now rather than trusting the argument: it is null on a
	// deselect, and it is a pointer into a set of rows that any refresh replaces.
	const TArray<TSharedPtr<FCrowdyModelSummary>> Selected =
		ModelListView.IsValid() ? ModelListView->GetSelectedItems() : TArray<TSharedPtr<FCrowdyModelSummary>>();

	if (Selected.Num() == 0 || !Selected[0].IsValid())
	{
		SelectedTypeName.Reset();
		ClearDetailPanel();
		UpdateModelDeleteControl();
		return;
	}

	SelectedTypeName = Selected[0]->TypeName;
	ShowInDetailPanel(*Selected[0]);
	UpdateModelDeleteControl();
}

void SCrowdyModelBrowserTab::UpdateModelDeleteControl()
{
	bModelMarkEnabled = false;
	ModelMarkLabel = LOCTEXT("MarkModel", "Mark for deletion");
	ModelMarkTooltip = LOCTEXT("MarkModelNoSelection", "Highlight a model to mark it for deletion.");

	FText GateReason;

	// Asked of the list view itself, every time. A control that remembered a model would stay armed at one that a
	// refresh has already replaced, and the app-identity check the controller makes cannot catch that, because
	// what has gone wrong there is target identity.
	if (ModelListView.IsValid() && ModelListView->GetNumItemsSelected() > 0 && DeletePanel.IsValid())
	{
		const TArray<TSharedPtr<FCrowdyModelSummary>> Selected = ModelListView->GetSelectedItems();
		if (Selected.Num() > 0 && Selected[0].IsValid())
		{
			const FCrowdyDeleteGate Gate = CrowdyGameModelDelete::CanDeleteFromPrimaryView(*Selected[0]);
			if (Gate.bAllowed)
			{
				const bool bAlreadyMarked = DeletePanel->IsMarked(CrowdyGameModelDelete::MarkFromModel(*Selected[0]));
				bModelMarkEnabled = true;
				ModelMarkLabel = bAlreadyMarked
					? LOCTEXT("UnmarkModel", "Unmark")
					: LOCTEXT("MarkModel", "Mark for deletion");
				ModelMarkTooltip = bAlreadyMarked
					? LOCTEXT("UnmarkModelTip", "Take this model out of the marked set. Nothing has been deleted.")
					: LOCTEXT("MarkModelTip", "Add this model to the marked set. Nothing is read or deleted until the review is opened.");
			}
			else
			{
				ModelMarkTooltip = FText::FromString(Gate.Reason);
				GateReason = FText::FromString(Gate.Reason);
			}
		}
	}

	if (MarkModelLabelText.IsValid())
	{
		MarkModelLabelText->SetText(ModelMarkLabel);
	}

	if (ModelGateReasonText.IsValid())
	{
		ModelGateReasonText->SetText(GateReason);
		ModelGateReasonText->SetVisibility(GateReason.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}
}

FReply SCrowdyModelBrowserTab::OnMarkModelClicked()
{
	// The target is read from the list at this instant, and handed over whole, so nothing between the row and the
	// mark re-types any of its fields.
	if (!ModelListView.IsValid() || ModelListView->GetNumItemsSelected() == 0 || !DeletePanel.IsValid())
	{
		return FReply::Handled();
	}

	const TArray<TSharedPtr<FCrowdyModelSummary>> Selected = ModelListView->GetSelectedItems();
	if (Selected.Num() == 0 || !Selected[0].IsValid())
	{
		return FReply::Handled();
	}

	// The same gate the control was drawn from, asked again: what a model is allowed to offer can change between
	// the moment the control was drawn and the moment it was pressed.
	if (!CrowdyGameModelDelete::CanDeleteFromPrimaryView(*Selected[0]).bAllowed)
	{
		UpdateModelDeleteControl();
		return FReply::Handled();
	}

	const FCrowdyDeleteMark Mark = CrowdyGameModelDelete::MarkFromModel(*Selected[0]);
	if (DeletePanel->IsMarked(Mark))
	{
		DeletePanel->UnmarkForDelete(Mark);
	}
	else
	{
		DeletePanel->MarkForDelete(Mark);
	}

	UpdateModelDeleteControl();
	return FReply::Handled();
}

FReply SCrowdyModelBrowserTab::OnReviewDeletionsClicked()
{
	if (DeletePanel.IsValid())
	{
		DeletePanel->OpenReview();
	}
	return FReply::Handled();
}

bool SCrowdyModelBrowserTab::IsRowMarkedForDelete(const FCrowdyModelRow& Row) const
{
	return DeletePanel.IsValid() && DeletePanel->IsMarked(CrowdyGameModelDelete::MarkFromRow(Row));
}

void SCrowdyModelBrowserTab::ToggleRowDeleteMark(const FCrowdyModelRow& Row)
{
	if (!DeletePanel.IsValid())
	{
		return;
	}

	const FCrowdyDeleteMark Mark = CrowdyGameModelDelete::MarkFromRow(Row);
	if (DeletePanel->IsMarked(Mark))
	{
		DeletePanel->UnmarkForDelete(Mark);
	}
	else
	{
		DeletePanel->MarkForDelete(Mark);
	}
}

void SCrowdyModelBrowserTab::HandleMarksChanged(int32 MarkCount)
{
	if (DeletePanelBox.IsValid())
	{
		DeletePanelBox->SetVisibility(MarkCount > 0 ? EVisibility::Visible : EVisibility::Collapsed);
	}

	if (ReviewButtonBox.IsValid())
	{
		ReviewButtonBox->SetVisibility(MarkCount > 0 ? EVisibility::Visible : EVisibility::Collapsed);
	}

	if (ReviewButtonLabelText.IsValid())
	{
		// Built here rather than in a text binding: the count moves only when the marked set does.
		ReviewButtonLabelText->SetText(FText::Format(
			LOCTEXT("ReviewDeletionsCount", "Review {0} deletions"), FText::AsNumber(MarkCount)));
	}

	// One marked set, so a row marked from either control reads as marked from both.
	UpdateModelDeleteControl();
	if (DetailPanel.IsValid())
	{
		DetailPanel->RefreshRowDeleteControl();
	}
}

void SCrowdyModelBrowserTab::HandleDeleteCommitFinished()
{
	// The controller has already re-read what the commit changed, so there is nothing to ask for here; what is on
	// screen was computed against the lists those reads replaced.
	MarkLedgerDirty();
}

void SCrowdyModelBrowserTab::HandleShowBlockingModel(const FString& TypeName)
{
	SelectModel(TypeName);
}

void SCrowdyModelBrowserTab::HandleShowCrossLink(
	const FString& TypeName, const FString& SectionKey, const FCrowdyModelRow& Entity)
{
	SelectModelRow(TypeName, SectionKey, Entity);
}

TSharedRef<ITableRow> SCrowdyModelBrowserTab::MakeModelRow(TSharedPtr<FCrowdyModelSummary> Model, const TSharedRef<STableViewBase>& OwnerTable)
{
	FString Title;
	FString Secondary;
	FString Tooltip;
	ECrowdyModelProvenance Provenance = ECrowdyModelProvenance::Unknown;

	if (Model.IsValid())
	{
		Title = Model->Display;
		Provenance = Model->Provenance;

		// The type name earns its place beside the display name: it is what a developer reading the class types
		// into the search box, and a display name can be anything at all.
		if (!Model->TypeName.IsEmpty() && !Model->TypeName.Equals(Model->Display, ESearchCase::CaseSensitive))
		{
			Secondary = Model->TypeName + TEXT("   ");
		}
		Secondary += CountPhrase(Model->FunctionCount, TEXT("function"), TEXT("functions"))
			+ TEXT(", ")
			+ CountPhrase(Model->AutomationCount, TEXT("automation"), TEXT("automations"));

		// A model with something wrong says so before it is opened, and leads with the word: this line is narrow
		// enough to end in an ellipsis, and the one part of it that cannot afford to be cut is the part that says
		// something needs attention. A model that matches the project adds nothing here at all.
		if (!Model->DriftText.IsEmpty())
		{
			Secondary = Model->DriftText + TEXT("  -  ") + Secondary;
		}

		Tooltip = Model->Description;
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	return SNew(STableRow<TSharedPtr<FCrowdyModelSummary>>, OwnerTable)
		.Style(&Style, "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		.ToolTipText(FText::FromString(Tooltip))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)

				// The same gutter mark the section tables carry, in the same column position on every row whatever
				// it has to say, so the titles beside it stay in one line.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 9.0f, 0.0f)
				[ SNew(SCrowdyProvenanceGlyph).Provenance(Provenance) ]

				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).Text(FText::FromString(Title)).TextStyle(&Style, "Crowdy.Text.BodyStrong").OverflowPolicy(ETextOverflowPolicy::Ellipsis) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[ SNew(STextBlock).Text(FText::FromString(Secondary)).TextStyle(&Style, "Crowdy.Text.Subtle").OverflowPolicy(ETextOverflowPolicy::Ellipsis) ]
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
