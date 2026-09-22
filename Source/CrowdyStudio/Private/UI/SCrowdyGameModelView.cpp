// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyGameModelView.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Types/WidgetActiveTimerDelegate.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/SCrowdyBusyBar.h"
#include "UI/GameModel/SCrowdyGameModelAdvancedTab.h"
#include "UI/GameModel/SCrowdyLiveModelsTab.h"
#include "UI/GameModel/SCrowdyModelBrowserTab.h"
#include "UI/GameModel/SCrowdyModelIssuesTab.h"
#include "UI/GameModel/SCrowdyPreSeedCard.h"
#include "UI/GameModel/SCrowdyReconcileStrip.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyGameModelView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyGameModelView::HandleAppChanged);
		Controller->OnSchemaPlanProgress.AddSP(this, &SCrowdyGameModelView::HandleSchemaPlanProgress);
		Controller->OnGameModelLintChanged.AddSP(this, &SCrowdyGameModelView::HandleLintChanged);
	}

	// Armed from the start so an app restored before this page was built is loaded the first time the page is opened.
	ScheduleEnsureLists();

	ActiveTab = Controller.IsValid() ? Controller->GetLastGameModelTab() : FString();
	// Availability is checked as well as existence: Issues can be the persisted key, and no lint has run yet at
	// construction, so restoring it verbatim would open a page behind a tab that cannot be selected back onto.
	if (TabIndexForKey(ActiveTab) == INDEX_NONE || !IsTabAvailable(ActiveTab))
	{
		ActiveTab = TEXT("models");
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> RefreshButton = SNew(SButton)
		.ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(9.0f, 5.0f))
		.ToolTipText(LOCTEXT("GameModelRefreshTip",
			"Re-read this app's models, attributes, functions, automations, features and policy from the server."))
		.OnClicked(this, &SCrowdyGameModelView::OnRefreshClicked)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ CrowdyStudioWidgets::Icon(TEXT("refresh"), 14.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(LOCTEXT("GameModelRefresh", "Refresh")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
		];

	// Asks the server whether this app's model hangs together. Distinct from Refresh, which re-reads what is there:
	// the findings are about relationships BETWEEN objects, so nothing on this page can show them.
	TSharedRef<SWidget> LintButton = SNew(SButton)
		.ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(9.0f, 5.0f))
		.ToolTipText(LOCTEXT("GameModelLintTip", "Check this app's game model for problems the server can see: a container bound to a type nobody defined, a function calling one that does not exist, a timer targeting something it may not invoke. Findings are listed in the log. Errors can quarantine the object they name, so it refuses to run until you write the definition again."))
		.OnClicked(this, &SCrowdyGameModelView::OnLintClicked)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ CrowdyStudioWidgets::Icon(TEXT("check"), 14.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(LOCTEXT("GameModelLint", "Lint")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
		];

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(LOCTEXT("GameModelHeader", "Game Model")).TextStyle(&Style, "Crowdy.Text.Title") ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ LintButton ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ RefreshButton ]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("GameModelPlane", "The design-time schema the runtime consumes. Game plane: needs a session sign-in that can mint an app token, so an org token will not do.")) ]

		// Reads a plan out loud while it runs. Indeterminate on purpose: a plan is a stream of container assets, then
		// a stream of effect assets, then several server round trips, and nothing here can put a fraction on that.
		// A bar claiming a percentage would be inventing one, so this reports only that work is happening and which
		// part of it. Collapsed when idle, which also stops the marquee's animation timer.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SAssignNew(PlanProgressRow, SHorizontalBox)
			.Visibility(EVisibility::Collapsed)

			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(120.0f).HeightOverride(4.0f)
				[
					// A sweeping pill, not a fill: nothing here can compute a fraction, so none is claimed.
					SNew(SCrowdyBusyBar)
				]
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SAssignNew(PlanProgressText, STextBlock)
				.TextStyle(&Style, "Crowdy.Text.Subtle")
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			]
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[ SNew(SCrowdyReconcileStrip).Controller(Controller) ]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			SNew(SCrowdyPreSeedCard).Controller(Controller)
			.Visibility(this, &SCrowdyGameModelView::GetPreSeedCardVisibility)
		]

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			CrowdyStudioWidgets::TabStrip(
				MakeTabItems(),
				TAttribute<FString>::CreateLambda([this]() { return ActiveTab; }),
				[this](const FString& TabKey) { OnTabSelected(TabKey); })
		]

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SAssignNew(TabSwitcher, SWidgetSwitcher)

			// Slot order is what TabIndexForKey maps onto; keep the two in step.
			+ SWidgetSwitcher::Slot()
			[ SAssignNew(ModelBrowserTab, SCrowdyModelBrowserTab).Controller(Controller) ]
			// Every tab fills the page area. A switcher arranges its active slot at the slot's alignment, so a slot
			// aligned to the top would hand its tab only the height that tab happens to want, which for a splitter
			// of two lists is however many rows they have already generated.
			+ SWidgetSwitcher::Slot()
			[
				SNew(SCrowdyLiveModelsTab).Controller(Controller)
				.OnShowModelInBrowser(this, &SCrowdyGameModelView::OnShowModelInBrowser)
			]
			+ SWidgetSwitcher::Slot()
			[ SNew(SCrowdyGameModelAdvancedTab).Controller(Controller) ]
			+ SWidgetSwitcher::Slot()
			[ SNew(SCrowdyModelIssuesTab).Controller(Controller) ]
		]
	];

	TabSwitcher->SetActiveWidgetIndex(TabIndexForKey(ActiveTab));
}

SCrowdyGameModelView::~SCrowdyGameModelView()
{
	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.RemoveAll(this);
		Controller->OnSchemaPlanProgress.RemoveAll(this);
		Controller->OnGameModelLintChanged.RemoveAll(this);
	}
}

void SCrowdyGameModelView::ScheduleEnsureLists()
{
	if (EnsureListsTimerHandle.IsValid())
	{
		return;
	}

	EnsureListsTimerHandle = RegisterActiveTimer(0.0f,
		FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyGameModelView::HandleEnsureLists));
}

EActiveTimerReturnType SCrowdyGameModelView::HandleEnsureLists(double InCurrentTime, float InDeltaTime)
{
	EnsureListsTimerHandle.Reset();

	if (Controller.IsValid())
	{
		// The controller decides whether anything is owed: it knows which app the lists hold, and a plan that has
		// already published this app's schema counts as having loaded it.
		Controller->EnsureGameModelListsLoaded();
	}

	return EActiveTimerReturnType::Stop;
}

void SCrowdyGameModelView::HandleAppChanged()
{
	ScheduleEnsureLists();

	// A plan for the previous app is superseded and will never repaint anything, so the indicator must not carry over
	// and describe work under the app the user just picked.
	HandleSchemaPlanProgress();

	// The switch emptied the lint report with the rest of the app-scoped state, so the Issues tab may have gone
	// inert while it was open. Same eviction as a re-lint that comes back clean.
	HandleLintChanged();
}

void SCrowdyGameModelView::HandleSchemaPlanProgress()
{
	if (!PlanProgressRow.IsValid() || !PlanProgressText.IsValid())
	{
		return;
	}

	const bool bBusy = Controller.IsValid() && Controller->IsSchemaPlanInFlight();
	PlanProgressRow->SetVisibility(bBusy ? EVisibility::HitTestInvisible : EVisibility::Collapsed);

	if (bBusy)
	{
		// Built here rather than in a text binding, so nothing formats a string on a frame where nothing changed.
		PlanProgressText->SetText(FText::Format(
			LOCTEXT("PlanProgressPhase", "{0}..."), FText::FromString(Controller->GetSchemaPlanPhase())));
	}
}

int32 SCrowdyGameModelView::TabIndexForKey(const FString& TabKey)
{
	if (TabKey == TEXT("models"))
	{
		return 0;
	}
	if (TabKey == TEXT("live"))
	{
		return 1;
	}
	if (TabKey == TEXT("advanced"))
	{
		return 2;
	}
	if (TabKey == TEXT("issues"))
	{
		return 3;
	}
	return INDEX_NONE;
}

bool SCrowdyGameModelView::HasModelIssues() const
{
	static const FStudioLintReport Never;
	const FStudioLintReport& Report = Controller.IsValid() ? Controller->GetGameModelLintReport() : Never;
	return CrowdyLintTabStateFor(Report) == ECrowdyLintTabState::HasFindings;
}

EVisibility SCrowdyGameModelView::GetPreSeedCardVisibility() const
{
	// A collapsed child takes no height and no padding from the box, so the Live tab gets the whole card's band
	// back for its instance list and values.
	return CrowdyPreSeedCardBelongsOnTab(ActiveTab) ? EVisibility::Visible : EVisibility::Collapsed;
}

bool SCrowdyGameModelView::IsTabAvailable(const FString& TabKey) const
{
	return TabKey != TEXT("issues") || HasModelIssues();
}

FText SCrowdyGameModelView::IssuesTabToolTip() const
{
	static const FStudioLintReport Never;
	const FStudioLintReport& Report = Controller.IsValid() ? Controller->GetGameModelLintReport() : Never;

	// One decision, taken in one place, so the hint and the availability can never disagree about which of the
	// three states this app is in.
	switch (CrowdyLintTabStateFor(Report))
	{
	case ECrowdyLintTabState::NeverRun:
		return LOCTEXT("GameModelTabIssuesNeverRan",
			"Nothing checked yet. Press Lint to ask the server whether this app's game model hangs together.");
	case ECrowdyLintTabState::Clean:
		return LOCTEXT("GameModelTabIssuesClean", "No issues. This app's game model hangs together.");
	case ECrowdyLintTabState::HasFindings:
		break;
	}
	return FText::Format(
		LOCTEXT("GameModelTabIssuesFound", "{0} error(s) and {1} warning(s) in this app's game model."),
		FText::AsNumber(Report.ErrorCount), FText::AsNumber(Report.WarningCount));
}

TArray<CrowdyStudioWidgets::FCrowdyTabItem> SCrowdyGameModelView::MakeTabItems()
{
	TArray<CrowdyStudioWidgets::FCrowdyTabItem> Tabs;

	auto Add = [&Tabs](const TCHAR* Key, const FText& Label)-> CrowdyStudioWidgets::FCrowdyTabItem&
	{
		CrowdyStudioWidgets::FCrowdyTabItem& Item = Tabs.AddDefaulted_GetRef();
		Item.Value = Key;
		Item.Label = Label;
		return Item;
	};

	Add(TEXT("models"), LOCTEXT("GameModelTabModels", "Models"));
	Add(TEXT("live"), LOCTEXT("GameModelTabLive", "Live"));
	Add(TEXT("advanced"), LOCTEXT("GameModelTabAdvanced", "Advanced"));

	// Last, and inert until the server has something to say. It keeps its place either way so the three tabs
	// before it never move under the cursor.
	CrowdyStudioWidgets::FCrowdyTabItem& Issues = Add(TEXT("issues"), LOCTEXT("GameModelTabIssues", "Issues"));
	Issues.IsEnabled = TAttribute<bool>::CreateSP(this, &SCrowdyGameModelView::HasModelIssues);
	Issues.ToolTip = TAttribute<FText>::CreateSP(this, &SCrowdyGameModelView::IssuesTabToolTip);

	return Tabs;
}

void SCrowdyGameModelView::HandleLintChanged()
{
	// The transition that has to be handled rather than left to the strip: the user is standing on Issues when a
	// re-lint comes back clean. The tab goes inert under them, and a switcher left pointing at it would show an
	// empty page behind a tab nothing can select again.
	if (ActiveTab == TEXT("issues") && !HasModelIssues())
	{
		OnTabSelected(TEXT("models"));
	}
}

void SCrowdyGameModelView::OnTabSelected(const FString& TabKey)
{
	const int32 Index = TabIndexForKey(TabKey);
	if (Index == INDEX_NONE)
	{
		return;
	}
	// Checked here rather than only on the button, because this is the chokepoint every route goes through: the
	// strip, the cross-tab link, and the persisted key restored at construction.
	if (!IsTabAvailable(TabKey))
	{
		return;
	}

	ActiveTab = TabKey;
	if (TabSwitcher.IsValid())
	{
		TabSwitcher->SetActiveWidgetIndex(Index);
	}
	if (Controller.IsValid())
	{
		Controller->SetLastGameModelTab(TabKey);
	}
}

void SCrowdyGameModelView::OnShowModelInBrowser(const FString& TypeName)
{
	// A model and nothing on it. One implementation for both, so the tab switch and the deferred scroll cannot
	// end up done one way here and another way there.
	ShowModelRowInBrowser(TypeName, FString(), FCrowdyModelRow());
}

void SCrowdyGameModelView::ShowModelRowInBrowser(
	const FString& TypeName, const FString& SectionKey, const FCrowdyModelRow& Entity)
{
	// Switch first: the Models tab's slot is collapsed until it is the active one, and a scroll-into-view issued
	// against a widget with no geometry yet is a no-op.
	OnTabSelected(TEXT("models"));

	if (ModelBrowserTab.IsValid())
	{
		ModelBrowserTab->SelectModelRow(TypeName, SectionKey, Entity);
	}
}

FReply SCrowdyGameModelView::OnRefreshClicked()
{
	if (Controller.IsValid())
	{
		Controller->FetchContainerTypes();
		Controller->FetchFunctions(FString());
		Controller->FetchAutomations();
		Controller->FetchFeatures();
		Controller->FetchTierFeatures();
		Controller->FetchAppAccessTiers();
		Controller->FetchRuntimePermissions();
		Controller->FetchGameModelPolicy();
	}
	return FReply::Handled();
}

FReply SCrowdyGameModelView::OnLintClicked()
{
	if (Controller.IsValid())
	{
		// Not quiet: this one was asked for, so a clean model is worth saying out loud.
		Controller->RunGameModelLint(false);
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
