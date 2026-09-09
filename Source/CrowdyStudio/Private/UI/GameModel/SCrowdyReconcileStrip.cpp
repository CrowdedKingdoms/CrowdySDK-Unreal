// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyReconcileStrip.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/GameModel/CrowdyGameModelWidgets.h"
#include "UI/GameModel/CrowdyReconcileSummary.h"
#include "UI/GameModel/SCrowdyApplyReviewPanel.h"
#include "UI/GameModel/SCrowdyProvenanceLegend.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// The floating panels. Wide enough that the report's longest lines and the key's sentences do not wrap into
	// ribbons, and tall enough to hold the key outright, since a key that has to be scrolled to be read is barely
	// a key. Both scroll past their limit rather than growing, so neither can run off the bottom of the window.
	constexpr float CrowdyDetailsPanelWidth = 760.0f;
	constexpr float CrowdyDetailsPanelMaxHeight = 420.0f;
	constexpr float CrowdyMarksPanelWidth = 620.0f;
	constexpr float CrowdyMarksPanelMaxHeight = 460.0f;
}

void SCrowdyReconcileStrip::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnSchemaSyncReportChanged.AddSP(this, &SCrowdyReconcileStrip::HandleSchemaSyncReportChanged);
	}
	HandleSchemaSyncReportChanged();

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	auto Btn = [&Style](const FText& Label, bool bPrimary, FOnClicked OnClick) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.ButtonStyle(&Style, bPrimary ? "Crowdy.Button.Primary" : "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(13.0f, 7.0f))
			.OnClicked(OnClick)
			[ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ];
	};

	// The readiness of the three things a synced Game Model needs, and how much the last check found, on one line
	// across the whole card.
	TSharedRef<SWidget> StatusRow =
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 18.0f, 0.0f)
		[ CrowdyGameModelWidgets::ReadinessPill(LOCTEXT("SetupApp", "App"), LOCTEXT("SetupAppMissing", "Not selected"),
			[this]() { return Controller.IsValid() ? Controller->GetAppReadiness() : ECrowdyStudioReadiness::Unknown; }) ]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 18.0f, 0.0f)
		[ CrowdyGameModelWidgets::ReadinessPill(LOCTEXT("SetupChannel", "Session channel"), LOCTEXT("SetupChannelMissing", "Missing"),
			[this]() { return Controller.IsValid() ? Controller->GetSessionChannelReadiness() : ECrowdyStudioReadiness::Unknown; }) ]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		// "Needs sync" is reserved for what the button beside it writes. Entities the server has and this project
		// does not are a review item, not sync work, and they say so in their own words rather than asking for a
		// press that would leave them exactly where they are. Two different states reach that advisory word, so it is
		// read from the report: a plan that never issued a read has not found anything extra, it has not looked.
		[ CrowdyGameModelWidgets::ReadinessPill(LOCTEXT("SetupSchema", "Schema"), LOCTEXT("SetupSchemaDrift", "Needs sync"),
			[this]() { return Controller.IsValid() ? Controller->GetSchemaReadiness() : ECrowdyStudioReadiness::Unknown; },
			TAttribute<FText>::CreateSP(this, &SCrowdyReconcileStrip::GetSchemaAdvisoryWord)) ]

		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[ SNew(SSpacer) ]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(this, &SCrowdyReconcileStrip::GetCachedCountLineText) ];

	// The full explanation is the buttons' tooltip rather than a paragraph on the card. Four lines of prose set the
	// width of everything beside them and bought nothing a reader could not get by hovering the control they are
	// already looking at.
	const FText SyncHelp = LOCTEXT("SchemaSyncHelpV2", "Reflect every Server Owned (CrowdyModel) attribute on your CrowdyContainer classes and every Crowdy Effect asset into this app's schema. Preview changes is a dry run; Sync to Server writes the container types, property definitions, and functions. It never deletes server state. If an effect needs the app's session channel, Sync to Server creates it for you.");

	TSharedRef<SWidget> ActionRow =
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SBox).ToolTipText(SyncHelp)
			[ Btn(LOCTEXT("PreviewChangesButton", "Preview changes"), false, FOnClicked::CreateSP(this, &SCrowdyReconcileStrip::OnPreviewClicked)) ]
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			// Anchored rather than turned into a drop-down: this is the page's one write control and it keeps the
			// primary button's own look, while what it opens is a panel over the page rather than a permanent
			// tenant of a strip that has no room to give.
			SNew(SBox).ToolTipText(SyncHelp)
			[
				SAssignNew(ApplyAnchor, SMenuAnchor)
				.Placement(MenuPlacement_BelowAnchor)
				.OnGetMenuContent(FOnGetContent::CreateSP(this, &SCrowdyReconcileStrip::MakeApplyPanel))
				[
					Btn(LOCTEXT("SyncToServerButton", "Sync to Server"), true, FOnClicked::CreateSP(this, &SCrowdyReconcileStrip::OnSyncClicked))
				]
			]
		]

		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[ SNew(SSpacer) ]

		// The two things a reader consults rather than acts on. They open over the page, so neither costs the
		// browser below a single pixel while it is closed, and both get real room when they are open.
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SAssignNew(DetailsButton, SComboButton)
			.ContentPadding(FMargin(10.0f, 5.0f))
			.ToolTipText(LOCTEXT("DetailsTip", "The full report from the last check or sync."))
			.OnGetMenuContent(FOnGetContent::CreateSP(this, &SCrowdyReconcileStrip::MakeDetailsPanel))
			.ButtonContent()
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor::UseForeground())
				.Text(this, &SCrowdyReconcileStrip::GetDetailsButtonLabel)
			]
		]

		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SAssignNew(MarksButton, SComboButton)
			.ContentPadding(FMargin(10.0f, 5.0f))
			.ToolTipText(LOCTEXT("MarksTip", "What the Source marks and the Status words on the Models tab mean."))
			.OnGetMenuContent(FOnGetContent::CreateSP(this, &SCrowdyReconcileStrip::MakeMarksPanel))
			.ButtonContent()
			[
				SNew(STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				.ColorAndOpacity(FSlateColor::UseForeground())
				.Text(LOCTEXT("MarksButton", "What do these marks mean?"))
			]
		];

	ChildSlot
	[
		CrowdyStudioWidgets::Card(
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[ StatusRow ]

			+ SVerticalBox::Slot().AutoHeight()
			[ ActionRow ],

			FMargin(16.0f, 12.0f))
	];
}

TSharedRef<SWidget> SCrowdyReconcileStrip::MakeDetailsPanel()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// The report grows one line per planned change, so a first sync of a real project runs to hundreds of lines.
	// Bounded and scrolled, so a long one fills the panel rather than running off the bottom of the window.
	return SNew(SBox)
		.WidthOverride(CrowdyDetailsPanelWidth)
		.MaxDesiredHeight(CrowdyDetailsPanelMaxHeight)
		.Padding(FMargin(12.0f))
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(STextBlock)
				.TextStyle(&Style, "Crowdy.Text.Body")
				.AutoWrapText(true)
				.Text(this, &SCrowdyReconcileStrip::GetSchemaSyncReportText)
			]
		];
}

TSharedRef<SWidget> SCrowdyReconcileStrip::MakeApplyPanel()
{
	// Built afresh on every press, so the review always opens on the plan that is pending right now. A panel held
	// between presses would carry a selection made against a plan that no longer exists.
	const TWeakPtr<SCrowdyReconcileStrip> WeakSelf = SharedThis(this);

	return SNew(SCrowdyApplyReviewPanel)
		.Controller(Controller)
		.OnDismiss(FSimpleDelegate::CreateLambda([WeakSelf]()
		{
			if (const TSharedPtr<SCrowdyReconcileStrip> Self = WeakSelf.Pin())
			{
				if (Self->ApplyAnchor.IsValid())
				{
					Self->ApplyAnchor->SetIsOpen(false);
				}
			}
		}));
}

TSharedRef<SWidget> SCrowdyReconcileStrip::MakeMarksPanel()
{
	return SNew(SBox)
		.WidthOverride(CrowdyMarksPanelWidth)
		.MaxDesiredHeight(CrowdyMarksPanelMaxHeight)
		.Padding(FMargin(14.0f, 12.0f))
		[
			SNew(SCrowdyProvenanceLegend)
		];
}

SCrowdyReconcileStrip::~SCrowdyReconcileStrip()
{
	if (Controller.IsValid())
	{
		Controller->OnSchemaSyncReportChanged.RemoveAll(this);
	}
}

FReply SCrowdyReconcileStrip::OnPreviewClicked()
{
	if (Controller.IsValid())
	{
		Controller->PlanSchemaSync();
	}
	return FReply::Handled();
}

FReply SCrowdyReconcileStrip::OnSyncClicked()
{
	// Opening the review IS the confirmation, and it is the only one. It names the app, lists every change,
	// lets a reader hold part of it back, and refuses a plan that moved while it was on screen; a Yes/No dialog in
	// front of it could do none of those and would mean no single change could prove either gate was working. A
	// plan that is not pending opens the review anyway, where the refusal says which press comes first.
	if (ApplyAnchor.IsValid())
	{
		ApplyAnchor->SetIsOpen(true);
	}
	return FReply::Handled();
}

void SCrowdyReconcileStrip::HandleSchemaSyncReportChanged()
{
	if (!Controller.IsValid())
	{
		CachedCountLine = TEXT("Not checked yet.");
		bDetailsWorthReading = false;
		return;
	}
	const FCrowdySchemaSyncReport& Report = Controller->GetSchemaSyncReport();
	CachedCountLine = CrowdyReconcileSummary::BuildCountLine(Report);

	// A report that carries a warning or a status note is worth reading, and the button says so instead of the
	// panel appearing by itself. The same rule as before decides it, so what counts as worth reading has not moved.
	bDetailsWorthReading = Report.bValid && CrowdyReconcileSummary::ShouldOpenDetails(Report);
}

FText SCrowdyReconcileStrip::GetCachedCountLineText() const
{
	return FText::FromString(CachedCountLine);
}

FText SCrowdyReconcileStrip::GetSchemaSyncReportText() const
{
	if (!Controller.IsValid())
	{
		return FText::GetEmpty();
	}
	const FCrowdySchemaSyncReport& Report = Controller->GetSchemaSyncReport();
	if (!Report.bValid)
	{
		return LOCTEXT("SyncNotPlannedV2",
			"Click \"Preview changes\" to reflect your CrowdyContainer classes and preview the changes.");
	}

	FString Text;

	// A durable outcome banner (a failed plan, or a partial apply) leads the panel and, for a dropped plan, replaces
	// the misleading zero-count "matches code" line below.
	const bool bFailedPlan = !Report.StatusNote.IsEmpty() && Report.UpsertCount() == 0 && !Report.bApplied;
	if (!Report.StatusNote.IsEmpty())
	{
		Text += Report.StatusNote;
		if (bFailedPlan)
		{
			return FText::FromString(Text);
		}
		Text += TEXT("\n\n");
	}

	Text += FString::Printf(
		TEXT("%s +%d type(s), ~%d type update(s), +%d property(s), ~%d property update(s), +%d function(s), ~%d function update(s)."),
		Report.bApplied ? TEXT("Applied:") : TEXT("Plan:"),
		Report.TypesToCreate, Report.TypesToUpdate, Report.PropsToCreate, Report.PropsToUpdate,
		Report.FunctionsToCreate, Report.FunctionsToUpdate);

	if (Report.UpsertCount() == 0 && !Report.bApplied)
	{
		// "Matches" is a claim about the server, and a warning is exactly where that claim breaks down: a difference a
		// sync cannot express (a cleared default, a dropped timer) plans nothing and shows up only as a warning. Say
		// there is nothing to send, and point at the warnings rather than talking over them.
		Text += Report.Warnings.Num() > 0
			? TEXT("\nNothing to sync. Read the warnings below before assuming the schema matches code.")
			: TEXT("\nSchema matches code. Nothing to sync.");
	}
	for (const FString& Line : Report.Lines)
	{
		Text += TEXT("\n  ") + Line;
	}
	if (Report.Warnings.Num() > 0)
	{
		Text += TEXT("\n\nWarnings:");
		for (const FString& Warning : Report.Warnings)
		{
			Text += TEXT("\n  ! ") + Warning;
		}
	}
	if (Report.ServerOnlyCount() > 0)
	{
		Text += FString::Printf(
			TEXT("\n\n%d server-only entity(s) (%d type(s), %d property(s), %d function(s), %d automation(s)) are not declared in code. The sync never deletes them; on the Models tab, use \"Mark everything only on the server\" and review what deleting them would do first."),
			Report.ServerOnlyCount(), Report.ServerOnlyTypeCount, Report.ServerOnlyPropCount, Report.ServerOnlyFunctionCount,
			Report.ServerOnlyAutomationCount);
	}
	return FText::FromString(Text);
}

FText SCrowdyReconcileStrip::GetSchemaAdvisoryWord() const
{
	if (Controller.IsValid() && Controller->GetSchemaSyncReport().bServerNotCompared)
	{
		return LOCTEXT("SetupSchemaNotCompared", "Not checked against the server");
	}
	return LOCTEXT("SetupSchemaServerOnly", "Extra on server, review");
}

FText SCrowdyReconcileStrip::GetDetailsButtonLabel() const
{
	// The marker is the only thing that still says "this one has something in it". It is a word rather than a
	// colour, because every other state on this page is told by a word too.
	return bDetailsWorthReading
		? LOCTEXT("ShowDetailsAttention", "Details (worth a look)")
		: LOCTEXT("ShowDetails", "Details");
}

#undef LOCTEXT_NAMESPACE
