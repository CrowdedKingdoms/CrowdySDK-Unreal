// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyDeleteReviewPanel.h"

#include "GameModel/CrowdyModelVocabulary.h"
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
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// The colour a severity word is written in, and very nearly the only ink on this panel. Everything else is the
	// page's neutral grey, so what appears in colour is exactly what needs attention. The names here are prefixed
	// because the unity build merges this module's translation units, and a bare helper name would redefine
	// another file's.
	FLinearColor CrowdyDeleteReviewSeverityColour(ECrowdyDeleteSeverity Severity)
	{
		switch (Severity)
		{
		case ECrowdyDeleteSeverity::Blocker: return FCrowdyStudioStyle::Danger();
		case ECrowdyDeleteSeverity::Caution: return FCrowdyStudioStyle::Warning();
		default:                             return FCrowdyStudioStyle::TextSubtle();
		}
	}

	TSharedRef<SButton> CrowdyDeleteReviewButton(const FText& Label, FOnClicked OnClicked)
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

}

void SCrowdyDeleteReviewPanel::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	MarksChanged = InArgs._OnMarksChanged;
	CommitFinished = InArgs._OnCommitFinished;
	ShowModel = InArgs._OnShowModel;

	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyDeleteReviewPanel::HandleAppChanged);
		Controller->OnContainerTypesChanged.AddSP(this, &SCrowdyDeleteReviewPanel::HandleSchemaListsChanged);
		Controller->OnFunctionsChanged.AddSP(this, &SCrowdyDeleteReviewPanel::HandleSchemaListsChanged);
		Controller->OnAutomationsChanged.AddSP(this, &SCrowdyDeleteReviewPanel::HandleSchemaListsChanged);
		// The per-type cache signal rather than the flat mirror's: this panel reads a model's attributes by type,
		// and the mirror belongs to whichever view asked for a type last.
		Controller->OnPropertyDefsCached.AddSP(this, &SCrowdyDeleteReviewPanel::HandleSchemaListsChanged);
		Controller->OnModelSnapshotChanged.AddSP(this, &SCrowdyDeleteReviewPanel::HandleModelSnapshotChanged);
		Controller->OnDeleteCommitProgress.AddSP(this, &SCrowdyDeleteReviewPanel::HandleCommitProgress);
		Controller->OnDeleteCommitFinished.AddSP(this, &SCrowdyDeleteReviewPanel::HandleCommitFinished);
	}

	// The app this panel's marks belong to, as of construction. The page is built long after a remembered app was
	// restored and selected, so without this the first read to arrive would read as an app switch.
	EditorAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> Header =
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SAssignNew(MarkedSummaryText, STextBlock)
				.TextStyle(&Style, "Crowdy.Text.Heading")
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				// The old bulk prune, in its new home. It marks and never commits, so it stands in front of the
				// same sheet and the same ladder that one row does. Disabled where a bulk mark would offer
				// something it must not, with the reason on the control rather than in a dialog after the click.
				SNew(SBox)
				.Visibility_Lambda([this]() { return bReviewOpen ? EVisibility::Collapsed : EVisibility::Visible; })
				.ToolTipText_Lambda([this]() { return BulkMarkReason; })
				[
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Secondary")
					.ContentPadding(FMargin(10.0f, 5.0f))
					.IsEnabled_Lambda([this]() { return bBulkMarkAllowed; })
					.OnClicked_Lambda([this]() { MarkEverythingServerOnly(); return FReply::Handled(); })
					[
						SNew(STextBlock)
						.Text(LOCTEXT("MarkEverythingServerOnly", "Mark everything only on the server"))
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SAssignNew(NoticeText, STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
			.Visibility(EVisibility::Collapsed)
		];

	TSharedRef<SWidget> MarkedPage =
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			CrowdyStudioWidgets::Card(
				SAssignNew(MarkListView, SListView<TSharedPtr<FCrowdyDeleteMark>>)
				.ListViewStyle(&Style, "Crowdy.TableView")
				.ListItemsSource(&MarkItems)
				.OnGenerateRow(this, &SCrowdyDeleteReviewPanel::MakeMarkRow)
				.SelectionMode(ESelectionMode::Single),
				FMargin(4.0f), /*bFlat*/ true)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				CrowdyDeleteReviewButton(LOCTEXT("UnmarkSelected", "Unmark"),
					FOnClicked::CreateSP(this, &SCrowdyDeleteReviewPanel::OnUnmarkSelectedClicked))
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				CrowdyDeleteReviewButton(LOCTEXT("ClearMarks", "Clear all"),
					FOnClicked::CreateSP(this, &SCrowdyDeleteReviewPanel::OnClearMarksClicked))
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SSpacer)
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				SAssignNew(ReviewButton, SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(10.0f, 5.0f))
				.ToolTipText(LOCTEXT("ReviewTip",
					"Check what these deletions would do before any of them runs. This is the only point at which anything is read."))
				.OnClicked(this, &SCrowdyDeleteReviewPanel::OnReviewClicked)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ReviewDeletions", "Review"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			]
		];

	// The sheet. It sits outside the scrolling half on purpose: a button reading "Delete 47 entries" is only a
	// confirmation for as long as the 47 is still on screen beside it.
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

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SAssignNew(AcknowledgeCheckBox, SCheckBox)
			.IsChecked(this, &SCrowdyDeleteReviewPanel::GetAcknowledgeState)
			.OnCheckStateChanged(this, &SCrowdyDeleteReviewPanel::OnAcknowledgeChanged)
			.Visibility(EVisibility::Collapsed)
			[
				SNew(SBox).Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
				[
					SAssignNew(AcknowledgeLabelText, STextBlock)
					.TextStyle(&Style, "Crowdy.Text.Body")
					.AutoWrapText(true)
				]
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SAssignNew(ProgressText, STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
			.Visibility(EVisibility::Collapsed)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().AutoWidth()
			[
				CrowdyDeleteReviewButton(LOCTEXT("BackToMarks", "Back to the marked list"),
					FOnClicked::CreateSP(this, &SCrowdyDeleteReviewPanel::OnCloseReviewClicked))
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SSpacer)
			]

			+ SHorizontalBox::Slot().AutoWidth()
			[
				// Built NON-FOCUSABLE, and nothing in this panel ever moves keyboard focus to it, so Enter pressed
				// anywhere on the page cannot fire it. A page whose most destructive control is one keypress away
				// from a text box the reader was typing in has no confirmation at all.
				SAssignNew(CommitButton, SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Secondary")
				.ContentPadding(FMargin(12.0f, 6.0f))
				.IsFocusable(false)
				.IsEnabled(this, &SCrowdyDeleteReviewPanel::IsCommitEnabled)
				.OnClicked(this, &SCrowdyDeleteReviewPanel::OnCommitClicked)
				[
					SAssignNew(CommitLabelText, STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
					.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
				]
			]
		],
		FMargin(14.0f, 12.0f));

	TSharedRef<SWidget> ReviewPage =
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SAssignNew(FindingsBox, SVerticalBox)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			Sheet
		];

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			Header
		]

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SAssignNew(PageSwitcher, SWidgetSwitcher)

			// Slot order is what EPage names; keep the two in step.
			+ SWidgetSwitcher::Slot()
			[
				CrowdyStudioWidgets::EmptyState(TEXT("server"), LOCTEXT("NothingMarked",
					"Nothing is marked for deletion.\nHighlight a model or a row and press Mark for deletion."))
			]

			+ SWidgetSwitcher::Slot()
			[
				MarkedPage
			]

			+ SWidgetSwitcher::Slot()
			[
				ReviewPage
			]
		]
	];

	UpdateHeader();
	UpdateBulkMarkAffordance();
	ShowPage(EPage::Empty);
}

SCrowdyDeleteReviewPanel::~SCrowdyDeleteReviewPanel()
{
	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.RemoveAll(this);
		Controller->OnContainerTypesChanged.RemoveAll(this);
		Controller->OnFunctionsChanged.RemoveAll(this);
		Controller->OnAutomationsChanged.RemoveAll(this);
		Controller->OnPropertyDefsCached.RemoveAll(this);
		Controller->OnModelSnapshotChanged.RemoveAll(this);
		Controller->OnDeleteCommitProgress.RemoveAll(this);
		Controller->OnDeleteCommitFinished.RemoveAll(this);
	}
}

void SCrowdyDeleteReviewPanel::ShowPage(EPage Page)
{
	if (PageSwitcher.IsValid())
	{
		PageSwitcher->SetActiveWidgetIndex(static_cast<int32>(Page));
	}
}

void SCrowdyDeleteReviewPanel::MarkForDelete(const FCrowdyDeleteMark& Mark)
{
	// A mark names an entity in one app and means nothing in another, so the set is emptied before anything joins
	// it if the selection has moved since the last one.
	SyncEditorAppScope();

	if (EditorAppId == 0)
	{
		return;
	}

	// Marking the same entity twice is a no-op rather than a duplicate: a host that cannot tell (a row rebuilt
	// under it, a keyboard repeat) must not be punished for asking again.
	if (CrowdyGameModelDelete::ContainsMark(Marks, Mark))
	{
		return;
	}

	Marks.Add(Mark);
	SetNotice(FText::GetEmpty());
	NotifyMarksChanged();
}

void SCrowdyDeleteReviewPanel::UnmarkForDelete(const FCrowdyDeleteMark& Mark)
{
	const int32 Before = Marks.Num();
	Marks.RemoveAll([&Mark](const FCrowdyDeleteMark& Held) { return Held == Mark; });

	if (Marks.Num() != Before)
	{
		NotifyMarksChanged();
	}
}

bool SCrowdyDeleteReviewPanel::IsMarked(const FCrowdyDeleteMark& Mark) const
{
	return CrowdyGameModelDelete::ContainsMark(Marks, Mark);
}

void SCrowdyDeleteReviewPanel::ClearMarks()
{
	if (Marks.Num() == 0)
	{
		return;
	}

	Marks.Reset();
	NotifyMarksChanged();
}

void SCrowdyDeleteReviewPanel::MarkEverythingServerOnly()
{
	SyncEditorAppScope();

	if (!Controller.IsValid() || EditorAppId == 0)
	{
		return;
	}

	const FCrowdyDeleteEvidence Fresh = GatherEvidence();

	FString Reason;
	if (!CrowdyGameModelDelete::CanMarkEverythingServerOnly(Fresh, Reason))
	{
		SetNotice(FText::FromString(Reason));
		UpdateBulkMarkAffordance();
		return;
	}

	const TArray<FCrowdyDeleteMark> Bulk = CrowdyGameModelDelete::MarkEverythingServerOnly(Fresh);

	int32 Added = 0;
	for (const FCrowdyDeleteMark& Mark : Bulk)
	{
		if (!CrowdyGameModelDelete::ContainsMark(Marks, Mark))
		{
			Marks.Add(Mark);
			++Added;
		}
	}

	SetNotice(Added == 0
		? LOCTEXT("BulkMarkAddedNothing", "Everything only on the server is already marked.")
		: FText::Format(
			LOCTEXT("BulkMarkAdded", "Marked {0} more. Nothing has been deleted: press Review to see what this would do."),
			FText::AsNumber(Added)));

	if (Added > 0)
	{
		NotifyMarksChanged();
	}
}

bool SCrowdyDeleteReviewPanel::CanMarkEverythingServerOnly(FString& OutReason) const
{
	if (!Controller.IsValid() || Controller->GetSelectedAppId() == 0)
	{
		OutReason = TEXT("Select an app first.");
		return false;
	}

	const FCrowdyDeleteEvidence Fresh = GatherEvidence();
	return CrowdyGameModelDelete::CanMarkEverythingServerOnly(Fresh, OutReason);
}

void SCrowdyDeleteReviewPanel::OpenReview()
{
	SyncEditorAppScope();

	if (!Controller.IsValid() || Marks.Num() == 0 || EditorAppId == 0)
	{
		return;
	}

	if (bPreflightInFlight)
	{
		// The check is already out over this set. A second press would stack a second set of reads over the same
		// models, and the two completions would then race to describe one sheet.
		return;
	}

	if (Controller->IsDeleteCommitInFlight())
	{
		SetNotice(LOCTEXT("CommitAlreadyRunning", "A delete is running. The review reopens when it ends."));
		return;
	}

	RefreshEvidence();

	// One probe for the whole marked set rather than one per mark, and each model named once however many times it
	// was marked. Marking itself read nothing; this is the one moment anything is read.
	TArray<FString> MarkedModels;
	for (const FCrowdyDeleteMark& Mark : Marks)
	{
		if (Mark.Kind != ECrowdyDeleteKind::Model)
		{
			continue;
		}

		const FString& TypeName = Mark.Name;
		const bool bAlreadyNamed = MarkedModels.ContainsByPredicate(
			[&TypeName](const FString& Held)
			{
				// Model names are server keys and the default string comparison folds case.
				return Held.Equals(TypeName, ESearchCase::CaseSensitive);
			});

		if (!bAlreadyNamed)
		{
			MarkedModels.Add(TypeName);
		}
	}

	bReviewOpen = true;
	bPreflightInFlight = true;
	PreflightAppId = EditorAppId;
	bAcknowledged = false;

	// The plan on screen is about to be replaced by one built from counts nobody has yet, so it is emptied first:
	// no stale headline or action label can then be read in the meantime.
	Plan = FCrowdyDeletePlan();
	ShownConsentToken.Reset();

	SetNotice(FText::GetEmpty());
	ShowPage(EPage::Review);
	ShowPreflightPending();

	const TWeakPtr<SCrowdyDeleteReviewPanel> WeakSelf = SharedThis(this);
	Controller->CountLiveModelsScoped(MarkedModels, CrowdyDeleteLiveModelProbeLimit,
		[WeakSelf](int64 InAppId, TArray<FCrowdyDeleteLiveCount>&& Counts)
		{
			// The completion outlives the click and can land after this panel has gone, so it is held weakly and
			// checked rather than captured outright.
			if (const TSharedPtr<SCrowdyDeleteReviewPanel> Self = WeakSelf.Pin())
			{
				Self->HandlePreflightCounts(InAppId, MoveTemp(Counts));
			}
		});
}

void SCrowdyDeleteReviewPanel::CloseReview()
{
	bReviewOpen = false;
	bAcknowledged = false;
	ShownConsentToken.Reset();

	// The marks survive. Closing is going back to the list, not cancelling what was chosen.
	ShowPage(Marks.Num() == 0 ? EPage::Empty : EPage::Marked);
	UpdateHeader();
	UpdateBulkMarkAffordance();
}

FCrowdyDeleteEvidence SCrowdyDeleteReviewPanel::GatherEvidence() const
{
	FCrowdyDeleteEvidence Out;

	if (!Controller.IsValid())
	{
		return Out;
	}

	Out.AppId = Controller->GetSelectedAppId();
	Out.Snapshot = Controller->GetModelSnapshot();

	if (!Out.Snapshot.IsValid() && Evidence.Snapshot.IsValid())
	{
		// A commit drops the retained verdict for the app it wrote to, because a verdict computed before a write
		// describes a server that has moved. The one already in hand is what the sheet on screen was judged
		// against, so it stays until a fresh one arrives: dropping it mid-review would leave the remainder of a
		// stopped commit with no way to finish, which is the one thing a stopped commit has to allow.
		Out.Snapshot = Evidence.Snapshot;
	}

	Out.Types = Controller->GetContainerTypes();
	// The app-wide function list, never the mirror that follows whatever model a view asked for last: a list
	// narrowed to one model would report that no other model has a function bound to it, which is the
	// bound-functions blocker answering from a list that was never asked the question.
	Out.Functions = Controller->GetUnfilteredFunctions();
	Out.Automations = Controller->GetAutomations();
	Out.AutomationTriggers = Controller->GetAutomationTriggers();

	// Asked of the controller, which knows whether each read actually LANDED for this app. Not inferred from the
	// captured plan: the plan survives an app switch on purpose while the three lists above are emptied by one, so
	// a plan taken before a switch would go on vouching for lists that were emptied after it, and an unread
	// function list taken for an empty one clears the bound-functions blocker for every model in the app at once.
	Out.bTypesRead = Controller->HasReadContainerTypes();
	Out.bFunctionsRead = Controller->HasReadFunctions();
	Out.bAutomationsRead = Controller->HasReadAutomations();

	// The pre-flight's counts, which nothing here re-reads. Dropping them would answer "how many live models does
	// this have" with Unknown, which is a blocker, so a read landing under an armed sheet would disarm it and the
	// stopped-commit banner would sit above a Delete button that could never run again.
	Out.LiveCounts = Evidence.LiveCounts;

	// The attributes of every model the marked set touches, from what has already been read. A model with no entry
	// has never been read, which is a different answer from a model with an empty one, and no read is issued here:
	// the one read this surface makes is the live-model probe.
	for (const FCrowdyDeleteMark& Mark : Marks)
	{
		const FString OwningModel = Mark.OwningModelName();
		if (OwningModel.IsEmpty())
		{
			continue;
		}

		const bool bAlreadyHeld = Out.AttributesByModel.ContainsByPredicate(
			[&OwningModel](const FCrowdyDeleteModelAttributes& Held)
			{
				return Held.TypeName.Equals(OwningModel, ESearchCase::CaseSensitive);
			});

		if (bAlreadyHeld)
		{
			continue;
		}

		if (const TArray<TSharedPtr<FStudioPropertyDef>>* Defs = Controller->GetPropertyDefsForType(OwningModel))
		{
			FCrowdyDeleteModelAttributes& Entry = Out.AttributesByModel.AddDefaulted_GetRef();
			Entry.TypeName = OwningModel;
			Entry.Attributes = *Defs;
		}
	}

	return Out;
}

void SCrowdyDeleteReviewPanel::RefreshEvidence()
{
	Evidence = GatherEvidence();
}

void SCrowdyDeleteReviewPanel::HandlePreflightCounts(int64 InAppId, TArray<FCrowdyDeleteLiveCount>&& Counts)
{
	bPreflightInFlight = false;

	if (!bReviewOpen)
	{
		// Closed while the probe was out. The marks survive, and pressing Review again reads afresh.
		return;
	}

	if (InAppId != PreflightAppId || InAppId != EditorAppId)
	{
		// These counts describe an app this panel is no longer showing. A sheet built from them would name the
		// wrong models, so the review closes rather than rendering one.
		CloseReview();
		SetNotice(LOCTEXT("PreflightAppMoved",
			"The app changed while the check was running, so nothing was deleted. Press Review again."));
		return;
	}

	Evidence.LiveCounts = MoveTemp(Counts);
	RebuildPlan();
}

void SCrowdyDeleteReviewPanel::RebuildPlan()
{
	if (!bReviewOpen || bPreflightInFlight)
	{
		return;
	}

	// An acknowledgement is consent to the cautions that were on screen when it was ticked and not to whatever
	// replaced them, so every rebuild drops it.
	bAcknowledged = false;

	Plan = CrowdyGameModelDelete::BuildPlan(Marks, Evidence);
	ShownConsentToken = Plan.ConsentToken;

	RebuildFindings();
	RebuildSheet();
}

void SCrowdyDeleteReviewPanel::RebuildMarkedList()
{
	// A list view holds its items by shared pointer and compares its selection by pointer identity, so the item
	// set is rebuilt whenever the marks move and whatever was highlighted is simply gone. Nothing here reads a
	// remembered selection afterwards; the unmark control reads the list at the moment of its own click.
	MarkItems.Reset();
	MarkItems.Reserve(Marks.Num());
	for (const FCrowdyDeleteMark& Mark : Marks)
	{
		MarkItems.Add(MakeShared<FCrowdyDeleteMark>(Mark));
	}

	if (MarkListView.IsValid())
	{
		MarkListView->ClearSelection();
		MarkListView->RequestListRefresh();
	}
}

TSharedRef<SWidget> SCrowdyDeleteReviewPanel::MakeFindingBlock(const FCrowdyDeleteFinding& Finding)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const bool bIsBlocker = Finding.Severity == ECrowdyDeleteSeverity::Blocker;

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

	Body->AddSlot().AutoHeight()
	[
		SNew(STextBlock)
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
		.ColorAndOpacity(FSlateColor(CrowdyDeleteReviewSeverityColour(Finding.Severity)))
		.Text(FText::FromString(
			CrowdyModelVocabulary::DeleteSeverityLabel(Finding.Severity)
			+ TEXT("   ")
			+ CrowdyModelVocabulary::DeleteFindingLabel(Finding.Kind)))
	];

	Body->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
	[
		SNew(STextBlock)
		.TextStyle(&Style, "Crowdy.Text.Body")
		.AutoWrapText(true)
		.Text(FText::FromString(Finding.Headline))
	];

	if (!Finding.Remedy.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
			.Text(FText::FromString(Finding.Remedy))
		];
	}

	// A blocker is followed rather than merely read: the model it is about is opened in the browser, where the
	// rows that have to go first are listed. The remedy above names which of them.
	const FString BlockingModel = Finding.Subject.OwningModelName();
	const bool bCanFollow = bIsBlocker && !BlockingModel.IsEmpty() && ShowModel.IsBound();

	if (Finding.References.Num() > 0 || bCanFollow)
	{
		// One line per finding, never one per reference: fourteen rows repeating one sentence is a wall the reader
		// skips rather than an explanation they read. A blocker's list opens itself, because a blocker is the one
		// kind that has to be acted on before anything at all can run.
		TSharedRef<SVerticalBox> ReferenceBox = SNew(SVerticalBox)
			.Visibility(bIsBlocker ? EVisibility::Visible : EVisibility::Collapsed);

		for (const FString& Reference : Finding.References)
		{
			ReferenceBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.TextStyle(&Style, "Crowdy.Text.Subtle")
				.AutoWrapText(true)
				.Text(FText::FromString(TEXT("- ") + Reference))
			];
		}

		if (Finding.bReferencesTruncated)
		{
			ReferenceBox->AddSlot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.TextStyle(&Style, "Crowdy.Text.Subtle")
				.AutoWrapText(true)
				.Text(FText::Format(LOCTEXT("ReferencesTruncated", "and {0} more not listed here."),
					FText::AsNumber(FMath::Max(0, Finding.ReferenceCount - Finding.References.Num()))))
			];
		}

		TSharedRef<SHorizontalBox> Controls = SNew(SHorizontalBox);

		if (Finding.References.Num() > 0)
		{
			const FText ShowLabel = FText::Format(
				LOCTEXT("ShowReferences", "Show {0}"), FText::AsNumber(Finding.ReferenceCount));
			const FText HideLabel = FText::Format(
				LOCTEXT("HideReferences", "Hide {0}"), FText::AsNumber(Finding.ReferenceCount));

			TSharedPtr<STextBlock> DisclosureLabel;
			TSharedRef<SWidget> DisclosureContent =
				SAssignNew(DisclosureLabel, STextBlock)
				.Text(bIsBlocker ? HideLabel : ShowLabel)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor::UseForeground());

			// The disclosure's state lives in the two widgets it switches rather than in a member, so a rebuilt
			// findings list always starts from the same place instead of from a flag that outlived the finding it
			// described. Both are held weakly: this list is replaced whenever the plan is.
			const TWeakPtr<SVerticalBox> WeakReferenceBox = ReferenceBox;
			const TWeakPtr<STextBlock> WeakLabel = DisclosureLabel;

			Controls->AddSlot().AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Ghost")
				.ContentPadding(FMargin(6.0f, 3.0f))
				.OnClicked_Lambda([WeakReferenceBox, WeakLabel, ShowLabel, HideLabel]()
				{
					const TSharedPtr<SVerticalBox> Box = WeakReferenceBox.Pin();
					if (Box.IsValid())
					{
						const bool bWasOpen = Box->GetVisibility() != EVisibility::Collapsed;
						Box->SetVisibility(bWasOpen ? EVisibility::Collapsed : EVisibility::Visible);
						if (const TSharedPtr<STextBlock> Label = WeakLabel.Pin())
						{
							Label->SetText(bWasOpen ? ShowLabel : HideLabel);
						}
					}
					return FReply::Handled();
				})
				[
					DisclosureContent
				]
			];
		}

		if (bCanFollow)
		{
			Controls->AddSlot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Ghost")
				.ContentPadding(FMargin(6.0f, 3.0f))
				.OnClicked(this, &SCrowdyDeleteReviewPanel::OnShowBlockingModelClicked, BlockingModel)
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("ShowBlockingModel", "Open {0}"), FText::FromString(BlockingModel)))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FSlateColor::UseForeground())
				]
			];
		}

		Body->AddSlot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f).HAlign(HAlign_Left)
		[
			Controls
		];

		Body->AddSlot().AutoHeight()
		[
			ReferenceBox
		];
	}

	return SNew(SBorder)
		.BorderImage(Style.GetBrush("Crowdy.Inset"))
		.Padding(FMargin(11.0f, 9.0f))
		[
			Body
		];
}

void SCrowdyDeleteReviewPanel::RebuildFindings()
{
	if (!FindingsBox.IsValid())
	{
		return;
	}

	FindingsBox->ClearChildren();

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// What a stopped commit did not run, above everything else, because it is what the next press is about. The
	// list is the controller's own remainder rather than a recomputed guess at one: it starts with the op that
	// failed, since that one did not complete.
	//
	// Shown only while it still describes what the button below would do. A remainder outlives the plan that
	// produced it, so clearing the marks and marking something else leaves "press Delete again to finish the
	// remaining 3" standing over a button that would run something else entirely.
	if (Controller.IsValid())
	{
		const FCrowdyDeleteOutcome& Outcome = Controller->GetLastDeleteOutcome();
		const TArray<FCrowdyDeleteOp>& Remainder = Controller->GetDeleteRemainder();

		if (Outcome.bStopped && Outcome.AppId == EditorAppId
			&& CrowdyGameModelDelete::RemainderAppliesTo(Remainder, Plan.Ops))
		{
			TSharedRef<SVerticalBox> RemainderBox = SNew(SVerticalBox);

			RemainderBox->AddSlot().AutoHeight()
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Warning()))
				.AutoWrapText(true)
				.Text(FText::FromString(CrowdyGameModelDelete::StopText(Outcome)))
			];

			for (const FCrowdyDeleteOp& Op : Remainder)
			{
				RemainderBox->AddSlot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.TextStyle(&Style, "Crowdy.Text.Body")
					.AutoWrapText(true)
					.Text(FText::FromString(Op.Describe))
				];
			}

			FindingsBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SNew(SBorder)
				.BorderImage(Style.GetBrush("Crowdy.Inset"))
				.Padding(FMargin(11.0f, 9.0f))
				[
					RemainderBox
				]
			];
		}
	}

	if (Plan.Findings.Num() == 0)
	{
		FindingsBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
			.Text(LOCTEXT("NoFindings", "Nothing else depends on what is about to be deleted."))
		];
		return;
	}

	for (const FCrowdyDeleteFinding& Finding : Plan.Findings)
	{
		FindingsBox->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			MakeFindingBlock(Finding)
		];
	}
}

FReply SCrowdyDeleteReviewPanel::OnShowBlockingModelClicked(FString TypeName)
{
	ShowModel.ExecuteIfBound(TypeName);
	return FReply::Handled();
}

void SCrowdyDeleteReviewPanel::RebuildSheet()
{
	const FCrowdyDeleteSheet& Sheet = Plan.Sheet;

	if (HeadlineText.IsValid())
	{
		HeadlineText->SetText(FText::FromString(Sheet.Headline));
	}
	if (AppLineText.IsValid())
	{
		AppLineText->SetText(FText::FromString(Sheet.AppLine));
	}
	if (CommitLabelText.IsValid())
	{
		CommitLabelText->SetText(FText::FromString(Sheet.ActionLabel));
	}

	if (CountsText.IsValid())
	{
		// Joined once, here, rather than in a bound attribute: none of it changes between two painted frames.
		FString Lines;
		for (const FString& Line : Sheet.CountLines)
		{
			Lines += Lines.IsEmpty() ? Line : LINE_TERMINATOR + Line;
		}
		for (const FString& Extra : { Sheet.ImpliedLine, Sheet.SubsumedLine })
		{
			if (!Extra.IsEmpty())
			{
				Lines += Lines.IsEmpty() ? Extra : LINE_TERMINATOR + Extra;
			}
		}

		CountsText->SetText(FText::FromString(Lines));
		CountsText->SetVisibility(Lines.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}

	if (BlockedReasonText.IsValid())
	{
		BlockedReasonText->SetText(FText::FromString(Sheet.BlockedReason));
		BlockedReasonText->SetVisibility(Sheet.BlockedReason.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}

	if (AcknowledgeLabelText.IsValid())
	{
		AcknowledgeLabelText->SetText(FText::FromString(Sheet.AcknowledgeLabel));
	}
	if (AcknowledgeCheckBox.IsValid())
	{
		// Shown only where the ladder asks for one. A tick that appears on every plan is a tick nobody reads.
		AcknowledgeCheckBox->SetVisibility(
			Sheet.AcknowledgeLabel.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
	}

	if (ProgressText.IsValid())
	{
		// Whatever the line last said was about the plan this one replaces, so it goes. The commit paths put
		// their own sentence back after they have rebuilt.
		ProgressText->SetVisibility(EVisibility::Collapsed);
	}
}

void SCrowdyDeleteReviewPanel::UpdateHeader()
{
	if (!MarkedSummaryText.IsValid())
	{
		return;
	}

	MarkedSummaryText->SetText(Marks.Num() == 0
		? LOCTEXT("MarkedNone", "Marked for deletion")
		: FText::Format(LOCTEXT("MarkedCount", "Marked for deletion: {0}"), FText::AsNumber(Marks.Num())));
}

void SCrowdyDeleteReviewPanel::UpdateBulkMarkAffordance()
{
	FString Reason;
	bBulkMarkAllowed = CanMarkEverythingServerOnly(Reason);
	BulkMarkReason = bBulkMarkAllowed
		? LOCTEXT("BulkMarkAllowedTip",
			"Mark everything this app has that the project does not declare. It marks; nothing is deleted until the sheet is confirmed.")
		: FText::FromString(Reason);
}

void SCrowdyDeleteReviewPanel::SetNotice(const FText& Message)
{
	if (!NoticeText.IsValid())
	{
		return;
	}

	NoticeText->SetText(Message);
	NoticeText->SetVisibility(Message.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible);
}

void SCrowdyDeleteReviewPanel::ShowPreflightPending()
{
	if (FindingsBox.IsValid())
	{
		FindingsBox->ClearChildren();
		FindingsBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle")
			.AutoWrapText(true)
			.Text(LOCTEXT("PreflightRunning", "Checking how many live models each marked model has..."))
		];
	}

	if (HeadlineText.IsValid())
	{
		HeadlineText->SetText(LOCTEXT("PreflightHeadline", "Working out what this would delete"));
	}
	if (AppLineText.IsValid())
	{
		AppLineText->SetText(FText::Format(LOCTEXT("PreflightAppLine", "App {0}"),
			FText::FromString(FString::Printf(TEXT("%lld"), EditorAppId))));
	}
	if (CommitLabelText.IsValid())
	{
		CommitLabelText->SetText(LOCTEXT("PreflightAction", "Delete"));
	}
	if (CountsText.IsValid())
	{
		CountsText->SetVisibility(EVisibility::Collapsed);
	}
	if (BlockedReasonText.IsValid())
	{
		BlockedReasonText->SetVisibility(EVisibility::Collapsed);
	}
	if (AcknowledgeCheckBox.IsValid())
	{
		AcknowledgeCheckBox->SetVisibility(EVisibility::Collapsed);
	}
	if (ProgressText.IsValid())
	{
		ProgressText->SetVisibility(EVisibility::Collapsed);
	}
}

bool SCrowdyDeleteReviewPanel::IsCommitEnabled() const
{
	if (!bReviewOpen || bPreflightInFlight || !Controller.IsValid())
	{
		return false;
	}

	if (Controller->IsDeleteCommitInFlight())
	{
		return false;
	}

	// A plan carrying a blocker is never committable, whatever else is true, and no stronger confirmation arms
	// it: clearing a blocker is a change to the marked set or a refresh.
	if (!Plan.bCommittable || Plan.Ops.Num() == 0)
	{
		return false;
	}

	// A reader who has not ticked the acknowledgement has not consented to the cautions it names.
	if (Plan.Ladder == ECrowdyDeleteLadder::Acknowledge && !bAcknowledged)
	{
		return false;
	}

	return true;
}

FReply SCrowdyDeleteReviewPanel::OnCommitClicked()
{
	if (!Controller.IsValid() || !bReviewOpen || bPreflightInFlight)
	{
		return FReply::Handled();
	}

	// TWO SOURCES, which is the whole point. The token on the left is built from the controller's lists AS THEY ARE
	// NOW, re-read at the moment of the click; the one on the right is the fingerprint of the plan the sheet was
	// drawn from, taken when the evidence held here was last gathered. Rebuilding from the SAME held evidence would
	// re-derive the same answer and agree with itself however far the server had moved, which is a guard that gates
	// nothing. A read that landed without waking this panel, or one it declined to act on while a commit was
	// running, moves the left side and not the right.
	const FCrowdyDeleteEvidence FreshEvidence = GatherEvidence();
	const FCrowdyDeletePlan Fresh = CrowdyGameModelDelete::BuildPlan(Marks, FreshEvidence);

	if (!Fresh.ConsentToken.Equals(ShownConsentToken, ESearchCase::CaseSensitive))
	{
		Evidence = FreshEvidence;
		Plan = Fresh;
		ShownConsentToken = Plan.ConsentToken;
		bAcknowledged = false;
		RebuildFindings();
		RebuildSheet();

		if (ProgressText.IsValid())
		{
			ProgressText->SetText(LOCTEXT("PlanMoved",
				"What this would delete changed while the sheet was on screen, so nothing was deleted. Read it again, then press Delete."));
			ProgressText->SetVisibility(EVisibility::Visible);
		}
		return FReply::Handled();
	}

	if (!Fresh.bCommittable || Fresh.Ops.Num() == 0)
	{
		return FReply::Handled();
	}

	if (Fresh.Ladder == ECrowdyDeleteLadder::Acknowledge && !bAcknowledged)
	{
		return FReply::Handled();
	}

	Plan = Fresh;

	if (ProgressText.IsValid())
	{
		ProgressText->SetText(LOCTEXT("CommitStarting", "Deleting..."));
		ProgressText->SetVisibility(EVisibility::Visible);
	}

	// EditorAppId is the app this panel's marks were made in, which is not necessarily the one selected at this
	// instant. The controller compares the plan's own app AND this one against the live selection, so the check
	// has two sources rather than one value agreeing with itself.
	Controller->CommitDeletePlan(Plan, EditorAppId);
	return FReply::Handled();
}

FReply SCrowdyDeleteReviewPanel::OnUnmarkSelectedClicked()
{
	// The target is read from the list at the moment of the click and never from anything remembered when the
	// control was drawn: rebuilding the list builds a whole new set of items and drops the vanished one silently.
	if (!MarkListView.IsValid() || MarkListView->GetNumItemsSelected() == 0)
	{
		return FReply::Handled();
	}

	const TArray<TSharedPtr<FCrowdyDeleteMark>> Selected = MarkListView->GetSelectedItems();
	if (Selected.Num() == 0 || !Selected[0].IsValid())
	{
		return FReply::Handled();
	}

	UnmarkForDelete(*Selected[0]);
	return FReply::Handled();
}

FReply SCrowdyDeleteReviewPanel::OnClearMarksClicked()
{
	ClearMarks();
	return FReply::Handled();
}

FReply SCrowdyDeleteReviewPanel::OnReviewClicked()
{
	OpenReview();
	return FReply::Handled();
}

FReply SCrowdyDeleteReviewPanel::OnCloseReviewClicked()
{
	CloseReview();
	return FReply::Handled();
}

void SCrowdyDeleteReviewPanel::OnAcknowledgeChanged(ECheckBoxState NewState)
{
	bAcknowledged = NewState == ECheckBoxState::Checked;
}

ECheckBoxState SCrowdyDeleteReviewPanel::GetAcknowledgeState() const
{
	return bAcknowledged ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

TSharedRef<ITableRow> SCrowdyDeleteReviewPanel::MakeMarkRow(
	TSharedPtr<FCrowdyDeleteMark> Mark, const TSharedRef<STableViewBase>& OwnerTable)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	// Phrased by the delete layer, not here, so the one sentence naming what the reader picked is written in the
	// same place that decides whether a scoping can be promised at all.
	const FString Line = Mark.IsValid() ? CrowdyGameModelDelete::DescribeMark(*Mark, Evidence) : FString();

	return SNew(STableRow<TSharedPtr<FCrowdyDeleteMark>>, OwnerTable)
		.Style(&Style, "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder)
			.BorderImage(FStyleDefaults::GetNoBrush())
			.Padding(FMargin(11.0f, 6.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Line))
				.ToolTipText(FText::FromString(Line))
				.TextStyle(&Style, "Crowdy.Text.Body")
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		];
}

void SCrowdyDeleteReviewPanel::NotifyMarksChanged()
{
	// The marked list about to be rebuilt names each entity, and whether a function's name may be shown with a
	// model beside it depends on the app's function list, so the evidence has to be current before the rows are.
	RefreshEvidence();
	RebuildMarkedList();
	UpdateHeader();
	UpdateBulkMarkAffordance();

	if (bReviewOpen)
	{
		if (Marks.Num() == 0)
		{
			CloseReview();
		}
		else if (!bPreflightInFlight)
		{
			// The sheet describes the marked set, so a marked set that moved is a sheet that has to be redrawn.
			RebuildPlan();
		}
	}
	else
	{
		ShowPage(Marks.Num() == 0 ? EPage::Empty : EPage::Marked);
	}

	MarksChanged.ExecuteIfBound(Marks.Num());
}

void SCrowdyDeleteReviewPanel::HandleAppChanged()
{
	SyncEditorAppScope();
	UpdateBulkMarkAffordance();
}

void SCrowdyDeleteReviewPanel::HandleSchemaListsChanged()
{
	// An app switch empties the controller's lists before anything else happens, so this is the earliest and the
	// most reliable point at which the switch is visible here.
	SyncEditorAppScope();

	if (Controller.IsValid() && Controller->IsDeleteCommitInFlight())
	{
		// A walk refreshes as it goes. Rebuilding under a running commit would replace the plan it is half way
		// through describing; the finished signal is where the review picks the thread up again.
		return;
	}

	UpdateBulkMarkAffordance();

	// Refreshed whether or not the review is open: the marked list names entities too, and its sentences are built
	// from these same lists.
	RefreshEvidence();

	if (bReviewOpen && !bPreflightInFlight)
	{
		// The rows behind a finding can move under an open sheet, and a sheet that does not follow them is a
		// promise about a server that has already changed.
		RebuildPlan();
	}
	else if (!bReviewOpen && MarkListView.IsValid())
	{
		// The rows are regenerated, not the item set: what a mark line may claim about a name has moved, but the
		// marked set has not, and rebuilding the items would drop the row the reader has highlighted for the Unmark
		// control to read at the moment of its click.
		MarkListView->RebuildList();
	}
}

void SCrowdyDeleteReviewPanel::HandleModelSnapshotChanged()
{
	HandleSchemaListsChanged();
}

void SCrowdyDeleteReviewPanel::HandleCommitProgress()
{
	if (!ProgressText.IsValid() || !Controller.IsValid() || !Controller->IsDeleteCommitInFlight())
	{
		return;
	}

	// Written on the announcement rather than in a bound attribute, so nothing formats a string on a frame where
	// nothing moved.
	ProgressText->SetText(FText::Format(LOCTEXT("CommitProgress", "Deleted {0} of {1}."),
		FText::AsNumber(Controller->GetDeleteCommitCompleted()),
		FText::AsNumber(Controller->GetDeleteCommitTotal())));
	ProgressText->SetVisibility(EVisibility::Visible);
}

void SCrowdyDeleteReviewPanel::HandleCommitFinished()
{
	if (!Controller.IsValid())
	{
		return;
	}

	const FCrowdyDeleteOutcome& Outcome = Controller->GetLastDeleteOutcome();

	if (Outcome.AppId != EditorAppId)
	{
		// A commit for an app this panel is not showing. Reporting it here would name entities under the wrong
		// app, so the host is still told to rebuild its rows and nothing else is claimed.
		CommitFinished.ExecuteIfBound();
		return;
	}

	if (Outcome.bStopped)
	{
		// Everything up to the stop is committed and every one of those deletes is idempotent, so the marks all
		// stay: pressing Delete again runs the same plan, the ops that already finished answer that there was
		// nothing there, and the walk carries on from where it stopped. The exact remainder is listed above the
		// findings, so what is left is read rather than guessed at.
		RefreshEvidence();
		RebuildPlan();

		if (ProgressText.IsValid())
		{
			ProgressText->SetText(FText::FromString(CrowdyGameModelDelete::StopText(Outcome)));
			ProgressText->SetVisibility(EVisibility::Visible);
		}
	}
	else
	{
		// Nothing marked is left on the server, so nothing is left marked.
		Marks.Reset();
		RefreshEvidence();
		CloseReview();
		NotifyMarksChanged();
		SetNotice(FText::FromString(CrowdyGameModelDelete::CompletionText(Outcome)));
	}

	CommitFinished.ExecuteIfBound();
}

void SCrowdyDeleteReviewPanel::SyncEditorAppScope()
{
	const int64 CurrentAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;
	if (CurrentAppId == EditorAppId)
	{
		return;
	}

	EditorAppId = CurrentAppId;

	// Everything held here names entities of the app it was read for: the marks, the evidence they were judged
	// against, and the review over both. A model name carried over from the previous app is read as one of the new
	// app's, so none of it survives the switch.
	Evidence = FCrowdyDeleteEvidence();
	Plan = FCrowdyDeletePlan();
	bPreflightInFlight = false;
	PreflightAppId = 0;

	CloseReview();
	SetNotice(FText::GetEmpty());

	Marks.Reset();
	NotifyMarksChanged();
}

#undef LOCTEXT_NAMESPACE
