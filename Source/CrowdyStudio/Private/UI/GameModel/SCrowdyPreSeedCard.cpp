#include "UI/GameModel/SCrowdyPreSeedCard.h"

#include "Misc/PackageName.h"
#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMenuAnchor.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	constexpr float PreSeedPanelWidth = 760.0f;
	constexpr float PreSeedPanelMaxHeight = 440.0f;

	TSharedRef<SWidget> PreSeedButton(const ISlateStyle& Style, const FText& Label, bool bPrimary, FOnClicked OnClick,
		TAttribute<bool> IsEnabled = true)
	{
		return SNew(SButton)
			.ButtonStyle(&Style, bPrimary ? "Crowdy.Button.Primary" : "Crowdy.Button.Secondary")
			.ContentPadding(FMargin(13.0f, 7.0f))
			.IsEnabled(IsEnabled)
			.OnClicked(OnClick)
			[ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ];
	}
}

void SCrowdyPreSeedCard::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	if (Controller.IsValid())
	{
		Controller->OnPreSeedReportChanged.AddSP(this, &SCrowdyPreSeedCard::HandleReportChanged);
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyPreSeedCard::HandleAppChanged);
	}

	TSharedPtr<FStudioSession> AppScope = MakeShared<FStudioSession>();
	AppScope->Name = TEXT("App (no session)");
	Scopes.Add(AppScope);
	SelectedScope = AppScope;
	HandleReportChanged();

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FText Help = LOCTEXT("PreSeedHelp", "Create the Game Model containers this map's placed entities will bind, before any player connects. Scan open map writes the map's container manifest beside the map. Preview reads the rows the server already has in the chosen scope. Apply creates only the missing ones and never deletes.");

	TSharedRef<SWidget> StatusRow =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Body").Text(this, &SCrowdyPreSeedCard::GetManifestLineText) ]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[ SNew(SSpacer) ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(this, &SCrowdyPreSeedCard::GetCountLineText) ];

	TSharedRef<SWidget> ActionRow =
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SBox).ToolTipText(Help)
			[ PreSeedButton(Style, LOCTEXT("PreSeedScan", "Scan open map"), false, FOnClicked::CreateSP(this, &SCrowdyPreSeedCard::OnScanClicked)) ]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SBox).WidthOverride(220.0f).ToolTipText(LOCTEXT("PreSeedScopeTip", "Where the rows live: the app itself, or one of its sessions. A session game must activate its session before its level entities register."))
			[
				SAssignNew(ScopeCombo, SComboBox<TSharedPtr<FStudioSession>>)
				.OptionsSource(&Scopes)
				.OnComboBoxOpening(FOnComboBoxOpening::CreateSP(this, &SCrowdyPreSeedCard::RefreshScopes))
				.OnGenerateWidget_Lambda([](TSharedPtr<FStudioSession> Scope)
				{
					const FString Label = Scope.IsValid()
						? (Scope->SessionId.IsEmpty() ? Scope->Name : FString::Printf(TEXT("%s  (%s)"), *Scope->Name, *Scope->SessionId))
						: FString();
					return SNew(STextBlock).Text(FText::FromString(Label));
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<FStudioSession> Scope, ESelectInfo::Type) { if (Scope.IsValid()) { SelectedScope = Scope; } })
				[ SNew(STextBlock).Text(this, &SCrowdyPreSeedCard::GetScopeLabel) ]
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[
			SNew(SBox).ToolTipText(Help)
			[ PreSeedButton(Style, LOCTEXT("PreSeedPreview", "Preview"), false, FOnClicked::CreateSP(this, &SCrowdyPreSeedCard::OnPreviewClicked)) ]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SBox).ToolTipText(Help)
			[
				SAssignNew(ApplyAnchor, SMenuAnchor)
				.Placement(MenuPlacement_BelowAnchor)
				.OnGetMenuContent(FOnGetContent::CreateSP(this, &SCrowdyPreSeedCard::MakeReviewPanel))
				[ PreSeedButton(Style, LOCTEXT("PreSeedApply", "Apply"), true, FOnClicked::CreateSP(this, &SCrowdyPreSeedCard::OnApplyClicked)) ]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[ SNew(SSpacer) ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
		[
			SAssignNew(DetailsButton, SComboButton)
			.ContentPadding(FMargin(10.0f, 5.0f))
			.ToolTipText(LOCTEXT("PreSeedDetailsTip", "The full report from the last scan, preview or apply."))
			.OnGetMenuContent(FOnGetContent::CreateSP(this, &SCrowdyPreSeedCard::MakeDetailsPanel))
			.ButtonContent()
			[
				SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground())
				.Text(LOCTEXT("PreSeedDetails", "Details"))
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

SCrowdyPreSeedCard::~SCrowdyPreSeedCard()
{
	if (Controller.IsValid())
	{
		Controller->OnPreSeedReportChanged.RemoveAll(this);
		Controller->OnSelectedAppChanged.RemoveAll(this);
	}
}

void SCrowdyPreSeedCard::HandleAppChanged()
{
	Scopes.SetNum(1);
	SelectedScope = Scopes[0];
	if (ScopeCombo.IsValid())
	{
		ScopeCombo->RefreshOptions();
	}
	// Announced after the app token is minted, so the new app's sessions can be listed now rather than on open.
	RefreshScopes();
}

FReply SCrowdyPreSeedCard::OnScanClicked()
{
	if (Controller.IsValid())
	{
		Controller->ScanOpenMapForPreSeed();
	}
	return FReply::Handled();
}

FReply SCrowdyPreSeedCard::OnPreviewClicked()
{
	if (Controller.IsValid())
	{
		Controller->PlanPreSeed(SelectedScope.IsValid() ? SelectedScope->SessionId : FString());
	}
	return FReply::Handled();
}

FReply SCrowdyPreSeedCard::OnApplyClicked()
{
	// The review is the confirmation: it shows every row and refuses a plan that moved while it was open.
	if (ApplyAnchor.IsValid())
	{
		ApplyAnchor->SetIsOpen(true);
	}
	return FReply::Handled();
}

TSharedRef<SWidget> SCrowdyPreSeedCard::MakeReviewPanel()
{
	const TWeakPtr<SCrowdyPreSeedCard> WeakSelf = SharedThis(this);
	return SNew(SCrowdyPreSeedReviewPanel)
		.Controller(Controller)
		.OnDismiss(FSimpleDelegate::CreateLambda([WeakSelf]()
		{
			if (const TSharedPtr<SCrowdyPreSeedCard> Self = WeakSelf.Pin())
			{
				if (Self->ApplyAnchor.IsValid())
				{
					Self->ApplyAnchor->SetIsOpen(false);
				}
			}
		}));
}

TSharedRef<SWidget> SCrowdyPreSeedCard::MakeDetailsPanel()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	return SNew(SBox).WidthOverride(PreSeedPanelWidth).MaxDesiredHeight(PreSeedPanelMaxHeight).Padding(FMargin(12.0f))
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Body").AutoWrapText(true).Text(this, &SCrowdyPreSeedCard::GetReportText) ]
		];
}

void SCrowdyPreSeedCard::HandleReportChanged()
{
	if (!Controller.IsValid())
	{
		CachedManifestLine = FText::GetEmpty();
		CachedCountLine = LOCTEXT("PreSeedNotChecked", "Not checked yet.");
		CachedReportText = FText::GetEmpty();
		return;
	}
	const FCrowdyPreSeedReport& Report = Controller->GetPreSeedReport();
	CachedManifestLine = Report.MapPackage.IsEmpty()
		? LOCTEXT("PreSeedNoManifest", "Pre-seed containers: scan the open map to build its manifest.")
		: FText::Format(LOCTEXT("PreSeedManifestLine", "Pre-seed containers for {0}: {1} manifest row(s)."),
			FText::FromString(FPackageName::GetShortName(Report.MapPackage)), Report.ManifestRowCount);
	CachedCountLine = FText::FromString(FCrowdyPreSeedPlan::BuildCountLine(Report));
	CachedReportText = FText::FromString(FCrowdyPreSeedPlan::BuildReportText(Report));
}

void SCrowdyPreSeedCard::RefreshScopes()
{
	if (!Controller.IsValid())
	{
		return;
	}
	const TWeakPtr<SCrowdyPreSeedCard> WeakSelf = SharedThis(this);
	Controller->FetchSessions([WeakSelf](const TArray<FStudioSession>& Sessions)
	{
		const TSharedPtr<SCrowdyPreSeedCard> Self = WeakSelf.Pin();
		if (!Self.IsValid())
		{
			return;
		}
		// The app entry stays first; the selection survives when its session is still listed, else falls back to it.
		const FString Keep = Self->SelectedScope.IsValid() ? Self->SelectedScope->SessionId : FString();
		Self->Scopes.SetNum(1);
		Self->SelectedScope = Self->Scopes[0];
		for (const FStudioSession& Session : Sessions)
		{
			TSharedPtr<FStudioSession> Scope = MakeShared<FStudioSession>(Session);
			if (Scope->Name.IsEmpty())
			{
				Scope->Name = Scope->Status.IsEmpty() ? TEXT("session") : Scope->Status;
			}
			Self->Scopes.Add(Scope);
			if (!Keep.IsEmpty() && Scope->SessionId == Keep)
			{
				Self->SelectedScope = Scope;
			}
		}
		// The list arrives while the menu is open. SetSelectedItem would go through the list's selection path,
		// which closes the menu on any selection, so only the options are refreshed; the card's own SelectedScope
		// is what the button shows and what Preview reads.
		if (Self->ScopeCombo.IsValid())
		{
			Self->ScopeCombo->RefreshOptions();
		}
	});
}

FText SCrowdyPreSeedCard::GetScopeLabel() const
{
	return SelectedScope.IsValid() ? FText::FromString(SelectedScope->Name) : LOCTEXT("PreSeedScopeApp", "App (no session)");
}

void SCrowdyPreSeedReviewPanel::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	OnDismiss = InArgs._OnDismiss;
	GenerationSeen = Controller.IsValid() ? Controller->GetPreSeedPlanGeneration() : 0;

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FCrowdyPreSeedReport& Report = Controller.IsValid() ? Controller->GetPreSeedReport() : FCrowdyPreSeedReport();
	const FText Headline = !Report.bValid
		? LOCTEXT("PreSeedReviewNoPlan", "No plan is pending. Click Preview first.")
		: FText::Format(LOCTEXT("PreSeedReviewHeadline", "{0} Scope: {1}. Rows marked + are created; = already exist; ? exist on the server with no placement and are left alone."),
			FText::FromString(FCrowdyPreSeedPlan::BuildCountLine(Report)),
			FText::FromString(Report.ScopeSessionId.IsEmpty() ? TEXT("app") : Report.ScopeSessionId));

	ChildSlot
	[
		SNew(SBox).WidthOverride(PreSeedPanelWidth).MaxDesiredHeight(PreSeedPanelMaxHeight).Padding(FMargin(12.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Body").AutoWrapText(true).Text(Headline) ]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true).Text(FText::FromString(FCrowdyPreSeedPlan::BuildReportText(Report))) ]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
				[
					// Not focusable: Enter pressed elsewhere on the page must never land on the one button that writes.
					SNew(SButton)
					.ButtonStyle(&Style, "Crowdy.Button.Primary")
					.ContentPadding(FMargin(13.0f, 7.0f))
					.IsFocusable(false)
					.IsEnabled(this, &SCrowdyPreSeedReviewPanel::CanCreate)
					.OnClicked(this, &SCrowdyPreSeedReviewPanel::OnCreateClicked)
					[ SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()).Text(this, &SCrowdyPreSeedReviewPanel::GetCreateButtonLabel) ]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[ PreSeedButton(Style, LOCTEXT("PreSeedReviewCancel", "Cancel"), false, FOnClicked::CreateSP(this, &SCrowdyPreSeedReviewPanel::OnCancelClicked)) ]
			]
		]
	];
}

bool SCrowdyPreSeedReviewPanel::CanCreate() const
{
	if (!Controller.IsValid() || Controller->IsPreSeedBusy())
	{
		return false;
	}
	const FCrowdyPreSeedReport& Report = Controller->GetPreSeedReport();
	return Report.bValid && Report.ToCreate > 0 && GenerationSeen == Controller->GetPreSeedPlanGeneration();
}

FText SCrowdyPreSeedReviewPanel::GetCreateButtonLabel() const
{
	const int32 ToCreate = Controller.IsValid() ? Controller->GetPreSeedReport().ToCreate : 0;
	return FText::Format(LOCTEXT("PreSeedReviewCreate", "Create {0} row(s)"), ToCreate);
}

FReply SCrowdyPreSeedReviewPanel::OnCreateClicked()
{
	if (Controller.IsValid())
	{
		Controller->ApplyPreSeed(GenerationSeen);
	}
	OnDismiss.ExecuteIfBound();
	return FReply::Handled();
}

FReply SCrowdyPreSeedReviewPanel::OnCancelClicked()
{
	OnDismiss.ExecuteIfBound();
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
