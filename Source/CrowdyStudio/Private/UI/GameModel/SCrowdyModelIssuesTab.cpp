// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/GameModel/SCrowdyModelIssuesTab.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/ISlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	FText CountPhrase(int32 Errors, int32 Warnings)
	{
		return FText::FromString(FString::Printf(TEXT("%d error%s, %d warning%s"),
			Errors, Errors == 1 ? TEXT("") : TEXT("s"),
			Warnings, Warnings == 1 ? TEXT("") : TEXT("s")));
	}
}

void SCrowdyModelIssuesTab::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnGameModelLintChanged.AddSP(this, &SCrowdyModelIssuesTab::RebuildFromReport);
		// An app switch empties the report, and this tab has to stop describing the app it was read for.
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyModelIssuesTab::RebuildFromReport);
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> RelintButton = SNew(SButton)
		.ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(9.0f, 5.0f))
		.ToolTipText(LOCTEXT("IssuesRelintTip", "Ask the server again. Every check is recomputed on demand, so a finding you have just fixed disappears here without anything else being touched."))
		.OnClicked(this, &SCrowdyModelIssuesTab::OnRelintClicked)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ CrowdyStudioWidgets::Icon(TEXT("refresh"), 14.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("IssuesRelint", "Re-run lint"))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
			]
		];

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			CrowdyStudioWidgets::SectionHeader(LOCTEXT("IssuesHeader", "Issues"), TEXT("check"), RelintButton)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			SAssignNew(SummaryText, STextBlock)
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.AutoWrapText(true)
		]

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SAssignNew(BodySwitcher, SWidgetSwitcher)

			// 0: findings
			+ SWidgetSwitcher::Slot()
			[
				CrowdyStudioWidgets::Card(
					SAssignNew(FindingsList, SListView<TSharedPtr<FStudioLintFinding>>)
					.ListItemsSource(&Findings)
					.SelectionMode(ESelectionMode::None)
					.OnGenerateRow(this, &SCrowdyModelIssuesTab::MakeFindingRow),
					FMargin(2.0f), true)
			]

			// 1: nothing to show, which is two different situations and says which.
			+ SWidgetSwitcher::Slot()
			[
				SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(20.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ CrowdyStudioWidgets::Icon(TEXT("check"), 30.0f, FSlateColor(FCrowdyStudioStyle::TextSubtle())) ]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					[
						SAssignNew(EmptyText, STextBlock)
						.TextStyle(&Style, "Crowdy.Text.Subtle")
						.Justification(ETextJustify::Center)
					]
				]
			]
		]
	];

	RebuildFromReport();
}

SCrowdyModelIssuesTab::~SCrowdyModelIssuesTab()
{
	if (Controller.IsValid())
	{
		Controller->OnGameModelLintChanged.RemoveAll(this);
		Controller->OnSelectedAppChanged.RemoveAll(this);
	}
}

void SCrowdyModelIssuesTab::RebuildFromReport()
{
	Findings.Reset();

	const FStudioLintReport Empty;
	const FStudioLintReport& Report = Controller.IsValid() ? Controller->GetGameModelLintReport() : Empty;

	Findings.Reserve(Report.Findings.Num());
	for (const FStudioLintFinding& Finding : Report.Findings)
	{
		Findings.Add(MakeShared<FStudioLintFinding>(Finding));
	}

	if (SummaryText.IsValid())
	{
		// Counted separately rather than summed: an error can quarantine its object, a warning never does.
		SummaryText->SetText(Report.bRan
			? FText::Format(LOCTEXT("IssuesSummaryRan",
				"{0}. Errors are provably broken and can stop the object running until you write its definition again; warnings are reported only."),
				CountPhrase(Report.ErrorCount, Report.WarningCount))
			: LOCTEXT("IssuesSummaryNeverRan",
				"This app's game model has not been checked yet."));
	}

	if (EmptyText.IsValid())
	{
		// "Nothing wrong" and "nobody has asked" are different answers, and an app that has never been linted
		// must not be told it is clean.
		EmptyText->SetText(Report.bRan
			? LOCTEXT("IssuesEmptyClean", "No issues. This app's game model hangs together.")
			: LOCTEXT("IssuesEmptyNeverRan", "Nothing checked yet.\nPress Lint on the page header, or Re-run lint above."));
	}

	if (FindingsList.IsValid())
	{
		FindingsList->RequestListRefresh();
	}
	if (BodySwitcher.IsValid())
	{
		BodySwitcher->SetActiveWidgetIndex(Findings.Num() > 0 ? 0 : 1);
	}
}

TSharedRef<ITableRow> SCrowdyModelIssuesTab::MakeFindingRow(TSharedPtr<FStudioLintFinding> Finding,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	if (!Finding.IsValid())
	{
		return SNew(STableRow<TSharedPtr<FStudioLintFinding>>, OwnerTable);
	}

	const bool bIsError = Finding->IsError();

	// Prefixed with the kind because subjects collide across kinds: an automation and a function can both be
	// called on_join, and a list showing both as "on_join" would be actively misleading about which is broken.
	const FString Subject = Finding->SubjectKind.IsEmpty()
		? Finding->Subject
		: Finding->SubjectKind + TEXT("/") + Finding->Subject;

	// A row that stands for many objects says so, rather than reading as one broken thing among forty.
	const FText CountSuffix = Finding->Count > 1
		? FText::FromString(FString::Printf(TEXT("  x%d"), Finding->Count))
		: FText::GetEmpty();

	TSharedRef<SVerticalBox> Body = SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				CrowdyStudioWidgets::Badge(
					bIsError ? LOCTEXT("IssuesBadgeError", "ERROR") : LOCTEXT("IssuesBadgeWarning", "WARNING"),
					bIsError ? CrowdyStudioWidgets::EBadgeTone::Danger : CrowdyStudioWidgets::EBadgeTone::Warning)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Subject))
				.TextStyle(&Style, "Crowdy.Text.BodyStrong")
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Finding->Code))
				.TextStyle(&Style, "Crowdy.Text.Subtle")
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(CountSuffix).TextStyle(&Style, "Crowdy.Text.Subtle")
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Finding->Message))
			.TextStyle(&Style, "Crowdy.Text.Body")
			.AutoWrapText(true)
		];

	// The remedy is the only part of a finding a developer can act on from where they are reading it, so it gets
	// its own line rather than being folded into the message or dropped for want of a column.
	if (!Finding->Remedy.IsEmpty())
	{
		Body->AddSlot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Finding->Remedy))
			.TextStyle(&Style, "Crowdy.Text.Subtle")
			.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Info()))
			.AutoWrapText(true)
		];
	}

	return SNew(STableRow<TSharedPtr<FStudioLintFinding>>, OwnerTable)
		.Style(&Style, "Crowdy.TableRow")
		.Padding(FMargin(10.0f, 9.0f))
		[
			Body
		];
}

FReply SCrowdyModelIssuesTab::OnRelintClicked()
{
	if (Controller.IsValid())
	{
		// Not quiet: this one was asked for, so a clean answer is worth saying out loud. It is also the moment
		// this tab empties itself and the strip takes its own tab away, which reads as a fault if nothing says why.
		Controller->RunGameModelLint(false);
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
