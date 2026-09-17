// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyProjectView.h"

#include "ConfigSync/FCrowdyConfigSync.h"
#include "Misc/MessageDialog.h"
#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/SCrowdyCreateAppDialog.h"
#include "Web/FCrowdyStudioLinks.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// "2026-06-20T14:03:11Z" as "2026-06-20"; anything unparseable is shown as sent.
	FString ShortDate(const FString& Iso)
	{
		FDateTime Parsed;
		return FDateTime::ParseIso8601(*Iso, Parsed) ? Parsed.ToString(TEXT("%Y-%m-%d")) : Iso;
	}

	FString DatacenterName(const FString& Code)
	{
		if (Code == TEXT("or")) { return TEXT("US West (Oregon)"); }
		if (Code == TEXT("va")) { return TEXT("US East (Virginia)"); }
		return Code.ToUpper();
	}

	FString Dash(const FString& Value)
	{
		return Value.IsEmpty() ? TEXT("-") : Value;
	}
}

void SCrowdyProjectView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnOrganizationsChanged.AddSP(this, &SCrowdyProjectView::HandleOrganizationsChanged);
		Controller->OnAppsChanged.AddSP(this, &SCrowdyProjectView::HandleAppsChanged);
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyProjectView::HandleAppsChanged);
	}

	switch (FCrowdyConfigSync::GetUdpProtocol())
	{
	case ECrowdyUDPProtocol::IPv4: UdpProtocolChoice = TEXT("IPv4"); break;
	case ECrowdyUDPProtocol::IPv6: UdpProtocolChoice = TEXT("IPv6"); break;
	default:                       UdpProtocolChoice = TEXT("Auto"); break;
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	auto ToolbarButton = [&Style](const TCHAR* IconName, const FText& Label, FOnClicked OnClicked, const TCHAR* ButtonStyle)
	{
		return SNew(SButton).ButtonStyle(&Style, ButtonStyle).ContentPadding(FMargin(11.0f, 6.0f)).OnClicked(OnClicked)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[ CrowdyStudioWidgets::Icon(IconName, 14.0f, FSlateColor::UseForeground()) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(Label).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			];
	};

	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(LOCTEXT("ProjectHeader", "Project")).TextStyle(&Style, "Crowdy.Text.Title") ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[ ToolbarButton(TEXT("refresh"), LOCTEXT("RefreshApps", "Refresh"), FOnClicked::CreateSP(this, &SCrowdyProjectView::OnRefreshClicked), TEXT("Crowdy.Button.Ghost")) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SBox)
					.IsEnabled_Lambda([this]() { return CreateOrgId() != 0; })
					.ToolTipText_Lambda([this]()
					{
						if (!Controller.IsValid() || Controller->GetOrganizations().Num() == 0) { return LOCTEXT("CreateAppNeedsSignIn", "Sign in to an organization first."); }
						if (CreateOrgId() == 0) { return LOCTEXT("CreateAppNeedsOrg", "Pick an organization in the list filter first; the new app is created there."); }
						return LOCTEXT("CreateAppReady", "Create a new app: name it, pick its datacenter, review, done.");
					})
					[ ToolbarButton(TEXT("plus"), LOCTEXT("CreateAppButton", "Create app"), FOnClicked::CreateSP(this, &SCrowdyProjectView::OnCreateAppClicked), TEXT("Crowdy.Button.Secondary")) ]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("ProjectSub", "Point this project at one of your apps, then sync its ids and endpoints into DefaultGame.ini.")) ]

			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SSplitter)
				.Style(&Style, "Crowdy.Splitter")
				.Orientation(Orient_Horizontal)
				.PhysicalSplitterHandleSize(3.0f)
				+ SSplitter::Slot().Value(0.32f).MinSize(220.0f)
				[ SNew(SBox).Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))[ BuildRail() ] ]
				+ SSplitter::Slot().Value(0.68f).MinSize(320.0f)
				[ SNew(SBox).Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))[ BuildDetail() ] ]
			]
		]
		+ SOverlay::Slot()
		[
			SAssignNew(CreateDialog, SCrowdyCreateAppDialog)
			.Controller(Controller)
			.Visibility(EVisibility::Collapsed)
			.OnCreated(FOnStudioAppCreated::CreateLambda([this](int64 AppId)
			{
				// A status or search filter would hide the app just made; the rail must show what the pane selects.
				Filter.Status.Reset();
				Filter.Search.Reset();
				if (SearchBox.IsValid()) { SearchBox->SetText(FText::GetEmpty()); }
				if (Controller.IsValid())
				{
					Controller->SelectApp(AppId);
				}
				RefreshVisibleApps();
			}))
		]
	];

	HandleOrganizationsChanged();
	RefreshVisibleApps();
}

SCrowdyProjectView::~SCrowdyProjectView()
{
	if (Controller.IsValid())
	{
		Controller->OnOrganizationsChanged.RemoveAll(this);
		Controller->OnAppsChanged.RemoveAll(this);
		Controller->OnSelectedAppChanged.RemoveAll(this);
	}
}

TSharedRef<SWidget> SCrowdyProjectView::BuildRail()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SAssignNew(OrgComboBox, SComboBox<TSharedPtr<FStudioOrg>>)
			.OptionsSource(&OrgOptions)
			.OnGenerateWidget(this, &SCrowdyProjectView::MakeOrgComboEntry)
			.OnSelectionChanged(this, &SCrowdyProjectView::OnOrgComboChanged)
			[ SNew(STextBlock).Text(this, &SCrowdyProjectView::GetSelectedOrgLabel).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary())) ]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SAssignNew(SearchBox, SSearchBox)
			.Style(&Style, "Crowdy.SearchBox")
			.HintText(LOCTEXT("SearchApps", "Search apps"))
			.DelayChangeNotificationsWhileTyping(true)
			.OnTextChanged_Lambda([this](const FText& T) { Filter.Search = T.ToString().TrimStartAndEnd(); RefreshVisibleApps(); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			CrowdyStudioWidgets::TabStrip(
				{ TEXT(""), TEXT("LIVE"), TEXT("DRAFT"), TEXT("ARCHIVED") },
				{ LOCTEXT("FilterAll", "All"), LOCTEXT("FilterLive", "Live"), LOCTEXT("FilterDraft", "Draft"), LOCTEXT("FilterArchived", "Archived") },
				TAttribute<FString>::CreateLambda([this]() { return Filter.Status; }),
				[this](const FString& V) { Filter.Status = V; RefreshVisibleApps(); })
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			CrowdyStudioWidgets::Card(
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SAssignNew(AppListView, SListView<TSharedPtr<FStudioApp>>)
					.ListItemsSource(&VisibleApps)
					.OnGenerateRow(this, &SCrowdyProjectView::MakeAppRow)
					.OnSelectionChanged(this, &SCrowdyProjectView::OnAppSelected)
					.SelectionMode(ESelectionMode::Single)
				]
				// Empty and loading are different answers; only the first is a statement about the account.
				+ SOverlay::Slot()
				[
					SNew(SBox).Visibility_Lambda([this]()
					{
						return (Controller.IsValid() && VisibleApps.Num() == 0 && !Controller->IsFetchingApps()) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						CrowdyStudioWidgets::EmptyState(TEXT("apps"), LOCTEXT("NoApps", "No apps match.\nClear the filters, or create one."))
					]
				]
				+ SOverlay::Slot()
				[
					SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
					.Visibility_Lambda([this]()
					{
						return (Controller.IsValid() && VisibleApps.Num() == 0 && Controller->IsFetchingApps()) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ SNew(SCircularThrobber).Radius(9.0f) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Text(LOCTEXT("LoadingApps", "Loading your apps...")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
					]
				],
				FMargin(4.0f), /*bFlat*/ true)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(2.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true)
			.Text_Lambda([this]()
			{
				if (!Controller.IsValid()) { return FText::GetEmpty(); }
				const int32 Total = Controller->GetApps().Num();
				FText Count = VisibleApps.Num() == Total
					? FText::Format(LOCTEXT("AppCountAll", "{0} {0}|plural(one=app,other=apps)"), FText::AsNumber(Total))
					: FText::Format(LOCTEXT("AppCountSome", "{0} of {1} apps"), FText::AsNumber(VisibleApps.Num()), FText::AsNumber(Total));
				const int32 Free = Controller->GetFreeAppsPerOrg();
				if (Filter.OrgId == 0 || Free <= 0) { return Count; }
				int32 Used = 0;
				for (const TSharedPtr<FStudioApp>& App : Controller->GetApps())
				{
					if (App.IsValid() && App->OrgId == Filter.OrgId && App->Status != TEXT("ARCHIVED")) { ++Used; }
				}
				return FText::Format(LOCTEXT("AppCountSlots", "{0}  ·  {1} of {2} free slots used"), Count, FText::AsNumber(Used), FText::AsNumber(Free));
			})
		];
}

TSharedRef<SWidget> SCrowdyProjectView::BuildDetail()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	const TSharedRef<SWidget> ChangeBadge = SNew(SBorder)
		.BorderImage(Style.GetBrush("Crowdy.Pill"))
		.BorderBackgroundColor_Lambda([this]()
		{
			FLinearColor C = ConfigChangeCount() > 0 ? FCrowdyStudioStyle::Gold() : FCrowdyStudioStyle::Success();
			C.A = 0.16f;
			return FSlateColor(C);
		})
		.Padding(FMargin(8.0f, 2.0f))
		[
			SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
			.Text_Lambda([this]()
			{
				const int32 N = ConfigChangeCount();
				return N > 0 ? FText::Format(LOCTEXT("NChanges", "{0} TO CHANGE"), FText::AsNumber(N)) : LOCTEXT("InSync", "IN SYNC");
			})
			.ColorAndOpacity_Lambda([this]() { return FSlateColor(ConfigChangeCount() > 0 ? FCrowdyStudioStyle::GoldBright() : FCrowdyStudioStyle::Success()); })
		];

	return SAssignNew(DetailSwitcher, SWidgetSwitcher)
		+ SWidgetSwitcher::Slot()
		[
			CrowdyStudioWidgets::Card(
				CrowdyStudioWidgets::EmptyState(TEXT("apps"), LOCTEXT("NoAppSelected", "Select an app to see its details and point this project at it.")),
				FMargin(16.0f), /*bFlat*/ true)
		]
		+ SWidgetSwitcher::Slot()
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[ BuildHeaderCard() ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 8.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						CrowdyStudioWidgets::TabStrip(
							{ TEXT("config"), TEXT("connection") },
							{ LOCTEXT("TabConfig", "Configuration"), LOCTEXT("TabConnection", "Connection") },
							TAttribute<FString>::CreateLambda([this]() { return ActiveTab; }),
							[this](const FString& V)
							{
								ActiveTab = V;
								if (TabSwitcher.IsValid()) { TabSwitcher->SetActiveWidgetIndex(V == TEXT("connection") ? 1 : 0); }
							})
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ ChangeBadge ]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(TabSwitcher, SWidgetSwitcher)
					+ SWidgetSwitcher::Slot()[ BuildConfigurationTab() ]
					+ SWidgetSwitcher::Slot()[ BuildConnectionTab() ]
				]
			]
		];
}

TSharedRef<SWidget> SCrowdyProjectView::Fact(const FText& Label, TFunction<FString()> Value, bool bCopyable, bool bMono)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Font(bMono ? FCoreStyle::GetDefaultFontStyle("Mono", 9) : FCoreStyle::GetDefaultFontStyle("Regular", 10))
			.ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary()))
			.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
			.Text_Lambda([Value]() { return FText::FromString(Dash(Value())); })
			.ToolTipText_Lambda([Value]() { return FText::FromString(Value()); })
		];
	if (bCopyable)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(2.0f, 0.0f, 0.0f, 0.0f)
		[
			SNew(SBox).Visibility_Lambda([Value]() { return Value().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			[ CrowdyStudioWidgets::CopyButton(TAttribute<FString>::CreateLambda(Value), Label) ]
		];
	}

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(Label).TextStyle(&Style, "Crowdy.Text.Subtle") ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)[ Row ];
}

TSharedRef<SWidget> SCrowdyProjectView::ChoiceMenu(const FText& Label, const TArray<FString>& Values, const TArray<FText>& Labels,
	TFunction<FString()> Current, TFunction<void(const FString&)> OnPick, TFunction<bool()> Enabled)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	TSharedPtr<SComboButton> Button;
	TSharedRef<SVerticalBox> Menu = SNew(SVerticalBox);

	SAssignNew(Button, SComboButton)
		.ButtonStyle(&Style, "Crowdy.Button.Secondary")
		.ContentPadding(FMargin(11.0f, 6.0f))
		.HasDownArrow(true)
		.IsEnabled_Lambda([Enabled]() { return Enabled(); })
		.ButtonContent()
		[
			SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground())
			.Text_Lambda([Label, Values, Labels, Current]()
			{
				const int32 Index = Values.IndexOfByKey(Current());
				return Index != INDEX_NONE ? FText::Format(LOCTEXT("ChoiceLabel", "{0}: {1}"), Label, Labels[Index]) : Label;
			})
		]
		.MenuContent()
		[
			SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Card")).Padding(4.0f)[ Menu ]
		];

	for (int32 I = 0; I < Values.Num(); ++I)
	{
		const FString Value = Values[I];
		TWeakPtr<SComboButton> WeakButton = Button;
		Menu->AddSlot().AutoHeight()
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(12.0f, 6.0f)).HAlign(HAlign_Left)
			.OnClicked_Lambda([Value, OnPick, WeakButton]()
			{
				if (const TSharedPtr<SComboButton> B = WeakButton.Pin()) { B->SetIsOpen(false); }
				OnPick(Value);
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Text(Labels[I]).Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				.ColorAndOpacity_Lambda([Value, Current]() { return FSlateColor(Current() == Value ? FCrowdyStudioStyle::Gold() : FCrowdyStudioStyle::TextPrimary()); })
			]
		];
	}
	return Button.ToSharedRef();
}

TSharedRef<SWidget> SCrowdyProjectView::BuildHeaderCard()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	auto App = [this]() { return SelectedApp(); };
	auto Str = [App](TFunction<FString(const FStudioApp&)> Get) { return [App, Get]() { const TSharedPtr<FStudioApp> A = App(); return A.IsValid() ? Get(*A) : FString(); }; };
	auto CanManage = [this]() { return Controller.IsValid() && Controller->CanManageApps(); };

	const TSharedRef<SWidget> ViewMode = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Heading").Text_Lambda([Str]() { return FText::FromString(Str([](const FStudioApp& A) { return A.Name; })()); }) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Pill")).Padding(FMargin(8.0f, 2.0f))
				.BorderBackgroundColor_Lambda([Str]()
				{
					FLinearColor C = CrowdyStudioWidgets::ColorForTone(CrowdyStudioWidgets::ToneForStatus(Str([](const FStudioApp& A) { return A.Status; })()));
					C.A = 0.16f;
					return FSlateColor(C);
				})
				[
					SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.Text_Lambda([Str]() { return FText::FromString(Str([](const FStudioApp& A) { return A.Status; })()); })
					.ColorAndOpacity_Lambda([Str]() { return FSlateColor(CrowdyStudioWidgets::ColorForTone(CrowdyStudioWidgets::ToneForStatus(Str([](const FStudioApp& A) { return A.Status; })()))); })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Pill")).Padding(FMargin(8.0f, 2.0f))
				.BorderBackgroundColor(FSlateColor(FLinearColor(FCrowdyStudioStyle::TextSecondary().R, FCrowdyStudioStyle::TextSecondary().G, FCrowdyStudioStyle::TextSecondary().B, 0.16f)))
				[
					SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
					.Text_Lambda([Str]() { return FText::FromString(Str([](const FStudioApp& A) { return A.Visibility; })()); })
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ CrowdyStudioWidgets::CopyChip(TAttribute<FString>::CreateLambda(Str([](const FStudioApp& A) { return FString::Printf(TEXT("%lld"), A.AppId); })), LOCTEXT("AppIdWhat", "app id")) ]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ CrowdyStudioWidgets::CopyChip(TAttribute<FString>::CreateLambda(Str([](const FStudioApp& A) { return A.Slug; })), LOCTEXT("SlugWhat", "slug")) ]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.Text_Lambda([Str]()
				{
					return FText::FromString(Str([](const FStudioApp& A)
					{
						const FString Org = A.OrgName.IsEmpty() ? FString::Printf(TEXT("org #%lld"), A.OrgId) : A.OrgName;
						return FString::Printf(TEXT("%s  ·  created %s  ·  updated %s"), *Org, *ShortDate(A.CreatedAt), *ShortDate(A.UpdatedAt));
					})());
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Body").AutoWrapText(true)
			.Text_Lambda([Str]() { return FText::FromString(Str([](const FStudioApp& A) { return A.Description; })()); })
			.Visibility_Lambda([Str]() { return Str([](const FStudioApp& A) { return A.Description; })().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
		[
			SNew(SUniformGridPanel).SlotPadding(FMargin(0.0f, 0.0f, 14.0f, 8.0f))
			+ SUniformGridPanel::Slot(0, 0)[ Fact(LOCTEXT("FactDc", "Datacenter"), Str([](const FStudioApp& A) { return A.DatacenterCode.IsEmpty() ? FString() : FString::Printf(TEXT("%s  %s"), *DatacenterName(A.DatacenterCode), *A.DatacenterCode); })) ]
			+ SUniformGridPanel::Slot(1, 0)[ Fact(LOCTEXT("FactRuntime", "Runtime"), Str([](const FStudioApp& A) { return A.RuntimeDenialReason.IsEmpty() ? A.RuntimeStatus : A.RuntimeStatus + TEXT(": ") + A.RuntimeDenialReason; })) ]
			+ SUniformGridPanel::Slot(2, 0)[ Fact(LOCTEXT("FactDeploy", "Deployment"), Str([](const FStudioApp& A) { return A.SplitMode == TEXT("true") ? A.DeploymentTarget + TEXT("  ·  split mode") : A.DeploymentTarget; })) ]
			+ SUniformGridPanel::Slot(0, 1)[ Fact(LOCTEXT("FactHttp", "Game API URL"), Str([](const FStudioApp& A) { return A.GameApiUrl; }), true, true) ]
			+ SUniformGridPanel::Slot(1, 1)[ Fact(LOCTEXT("FactWs", "Game API WS URL"), Str([](const FStudioApp& A) { return A.GameApiWsUrl; }), true, true) ]
			+ SUniformGridPanel::Slot(2, 1)[ Fact(LOCTEXT("FactOrgId", "Org ID"), Str([](const FStudioApp& A) { return FString::Printf(TEXT("%lld"), A.OrgId); }), true, true) ]
			+ SUniformGridPanel::Slot(0, 2)[ Fact(LOCTEXT("FactUdp", "Reserved UDP"), Str([](const FStudioApp& A) { return FString::Printf(TEXT("%lld B/s"), A.ReservedUdpBytesPerSec); })) ]
			+ SUniformGridPanel::Slot(1, 2)[ Fact(LOCTEXT("FactGql", "Reserved GraphQL"), Str([](const FStudioApp& A) { return FString::Printf(TEXT("%lld ops/s"), A.ReservedGraphqlOpsPerSec); })) ]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(11.0f, 6.0f))
				.IsEnabled_Lambda(CanManage)
				.ToolTipText(LOCTEXT("EditTip", "Rename the app or change its description."))
				.OnClicked_Lambda([this]()
				{
					const TSharedPtr<FStudioApp> A = SelectedApp();
					if (!A.IsValid()) { return FReply::Handled(); }
					if (EditNameBox.IsValid()) { EditNameBox->SetText(FText::FromString(A->Name)); }
					if (EditDescriptionBox.IsValid()) { EditDescriptionBox->SetText(FText::FromString(A->Description)); }
					EditingAppId = A->AppId;
					if (HeaderSwitcher.IsValid()) { HeaderSwitcher->SetActiveWidgetIndex(1); }
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ CrowdyStudioWidgets::Icon(TEXT("edit"), 14.0f, FSlateColor::UseForeground()) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Text(LOCTEXT("EditDetails", "Edit details")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				ChoiceMenu(LOCTEXT("StatusMenu", "Status"),
					{ TEXT("DRAFT"), TEXT("LIVE") }, { LOCTEXT("StDraft", "Draft"), LOCTEXT("StLive", "Live") },
					Str([](const FStudioApp& A) { return A.Status; }),
					[this](const FString& V)
					{
						if (const TSharedPtr<FStudioApp> A = SelectedApp()) { Controller->UpdateApp(A->AppId, FString(), TOptional<FString>(), V, FString()); }
					},
					CanManage)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				ChoiceMenu(LOCTEXT("VisibilityMenu", "Visibility"),
					{ TEXT("PRIVATE"), TEXT("UNLISTED"), TEXT("PUBLIC") }, { LOCTEXT("VisPrivate", "Private"), LOCTEXT("VisUnlisted", "Unlisted"), LOCTEXT("VisPublic", "Public") },
					Str([](const FStudioApp& A) { return A.Visibility; }),
					[this](const FString& V)
					{
						if (const TSharedPtr<FStudioApp> A = SelectedApp()) { Controller->UpdateApp(A->AppId, FString(), TOptional<FString>(), FString(), V); }
					},
					CanManage)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(9.0f, 6.0f))
				.ToolTipText(LOCTEXT("ConsoleTip", "Open this org's apps in the web console (members, tokens, billing, usage)."))
				.OnClicked_Lambda([this]()
				{
					if (const TSharedPtr<FStudioApp> A = SelectedApp())
					{
						FCrowdyStudioLinks::OpenExternal(A->OrgSlug.IsEmpty() ? FCrowdyStudioLinks::Dashboard() : FCrowdyStudioLinks::OrgTab(A->OrgSlug, TEXT("apps")));
					}
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ CrowdyStudioWidgets::Icon(TEXT("external-link"), 14.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Text(LOCTEXT("WebConsole", "Web console")).TextStyle(&Style, "Crowdy.Text.Body") ]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(9.0f, 6.0f))
				.IsEnabled_Lambda([this, CanManage]() { const TSharedPtr<FStudioApp> A = SelectedApp(); return CanManage() && A.IsValid() && A->Status != TEXT("ARCHIVED"); })
				.ToolTipText(LOCTEXT("ArchiveTip", "Archive the app. Reversible: set its status back to Draft or Live later."))
				.OnClicked(this, &SCrowdyProjectView::OnArchiveClicked)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)[ CrowdyStudioWidgets::Icon(TEXT("archive"), 14.0f, FSlateColor(FCrowdyStudioStyle::Danger())) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Text(LOCTEXT("Archive", "Archive")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 10)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger())) ]
				]
			]
		];

	const TSharedRef<SWidget> EditMode = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[ CrowdyStudioWidgets::Field(LOCTEXT("EditNameLabel", "Name"), SAssignNew(EditNameBox, SEditableTextBox).Style(&Style, "Crowdy.Input")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			CrowdyStudioWidgets::Field(LOCTEXT("EditDescLabel", "Description"),
				SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(2.0f))
				[ SNew(SBox).HeightOverride(60.0f)[ SAssignNew(EditDescriptionBox, SMultiLineEditableTextBox) ] ])
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(11.0f, 6.0f))
				.OnClicked_Lambda([this]() { EditingAppId = 0; if (HeaderSwitcher.IsValid()) { HeaderSwitcher->SetActiveWidgetIndex(0); } return FReply::Handled(); })
				[ SNew(STextBlock).Text(LOCTEXT("Cancel", "Cancel")).TextStyle(&Style, "Crowdy.Text.Body") ]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(11.0f, 6.0f))
				.IsEnabled_Lambda([this]() { return EditNameBox.IsValid() && !EditNameBox->GetText().ToString().TrimStartAndEnd().IsEmpty(); })
				.OnClicked(this, &SCrowdyProjectView::OnSaveDetailsClicked)
				[ SNew(STextBlock).Text(LOCTEXT("SaveDetails", "Save")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			]
		];

	return CrowdyStudioWidgets::Card(
		SAssignNew(HeaderSwitcher, SWidgetSwitcher)
		+ SWidgetSwitcher::Slot()[ ViewMode ]
		+ SWidgetSwitcher::Slot()[ EditMode ],
		FMargin(14.0f, 12.0f));
}

TSharedRef<SWidget> SCrowdyProjectView::BuildConfigurationTab()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// One setting: label, the value a sync would write (gold when it differs, with a copy button), and a dimmed
	// "was" line only when it actually changes.
	auto SettingRow = [](const FText& Label, TFunction<FString()> GetCurrent, TFunction<FString()> GetProposed) -> TSharedRef<SWidget>
	{
		const ISlateStyle& S = FCrowdyStudioStyle::Get();
		const FSlateFontInfo Mono = FCoreStyle::GetDefaultFontStyle("Mono", 9);
		auto Changed = [GetCurrent, GetProposed]() { return GetCurrent() != GetProposed(); };

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 1.0f, 18.0f, 0.0f)
			[ SNew(SBox).WidthOverride(140.0f)[ SNew(STextBlock).Text(Label).AutoWrapText(true).TextStyle(&S, "Crowdy.Text.Body") ] ]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).Font(Mono).AutoWrapText(true)
						.Text_Lambda([GetProposed]() { return FText::FromString(Dash(GetProposed())); })
						.ColorAndOpacity_Lambda([Changed]() { return Changed() ? FSlateColor(FCrowdyStudioStyle::GoldBright()) : FSlateColor(FCrowdyStudioStyle::TextSecondary()); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
					[ CrowdyStudioWidgets::CopyButton(TAttribute<FString>::CreateLambda(GetProposed), Label) ]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox).Visibility_Lambda([Changed]() { return Changed() ? EVisibility::Visible : EVisibility::Collapsed; })
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[ SNew(STextBlock).Text(LOCTEXT("WasLabel", "was")).TextStyle(&S, "Crowdy.Text.Subtle") ]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[ SNew(STextBlock).Font(Mono).AutoWrapText(true).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSubtle())).Text_Lambda([GetCurrent]() { return FText::FromString(Dash(GetCurrent())); }) ]
				]
			];
	};

	auto Divider = [&Style]() -> TSharedRef<SWidget>
	{
		return SNew(SBox).HeightOverride(1.0f)[ SNew(SImage).Image(Style.GetBrush("Crowdy.Separator")) ];
	};

	return CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body").Text(LOCTEXT("ConfigExplainer", "What the selected app writes into DefaultGame.ini: its app id, its org, and the game endpoints from the app's own routing. The shared origin comes from the Backend selector on the sign-in page. Changed values are gold.")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(14.0f, 12.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[ SettingRow(LOCTEXT("RowAppId", "App ID"),
					[this]() { return FString::Printf(TEXT("%lld"), Controller->GetCurrentSettings().AppId); },
					[this]() { return FString::Printf(TEXT("%lld"), Controller->BuildProposedSettings().AppId); }) ]
				+ SVerticalBox::Slot().AutoHeight()[ Divider() ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f)
				[ SettingRow(LOCTEXT("RowOrgId", "Org ID"),
					[this]() { return FString::Printf(TEXT("%lld"), Controller->GetCurrentSettings().OrgId); },
					[this]() { return FString::Printf(TEXT("%lld"), Controller->BuildProposedSettings().OrgId); }) ]
				+ SVerticalBox::Slot().AutoHeight()[ Divider() ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f)
				[ SettingRow(LOCTEXT("RowGameHttp", "Game API HTTP URL"),
					[this]() { return Controller->GetCurrentSettings().GameApiHttpUrl; },
					[this]() { return Controller->BuildProposedSettings().GameApiHttpUrl; }) ]
				+ SVerticalBox::Slot().AutoHeight()[ Divider() ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f)
				[ SettingRow(LOCTEXT("RowGameWs", "Game API WS URL"),
					[this]() { return Controller->GetCurrentSettings().GameApiWsUrl; },
					[this]() { return Controller->BuildProposedSettings().GameApiWsUrl; }) ]
				+ SVerticalBox::Slot().AutoHeight()[ Divider() ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[ SettingRow(LOCTEXT("RowDiscovery", "Shared origin"),
					[this]() { return Controller->GetCurrentSettings().DiscoveryUrl; },
					[this]() { return Controller->GetCurrentSettings().DiscoveryUrl; }) ]
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true).Text(LOCTEXT("SyncHint", "Written to DefaultGame.ini; running PIE sessions pick it up immediately, a fresh Play always does.")) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&Style, "Crowdy.Button.Primary")
				.ContentPadding(FMargin(14.0f, 8.0f))
				.IsEnabled_Lambda([this]() { return SelectedApp().IsValid(); })
				.OnClicked(this, &SCrowdyProjectView::OnSyncClicked)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)[ CrowdyStudioWidgets::Icon(TEXT("check"), 15.0f, FSlateColor::UseForeground()) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Text(LOCTEXT("SyncToProjectButton", "Sync to project")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
				]
			]
		],
		FMargin(16.0f, 14.0f));
}

TSharedRef<SWidget> SCrowdyProjectView::BuildConnectionTab()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	return CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body").Text(LOCTEXT("ConnExplainer", "Realtime UDP tuning, written to DefaultGame.ini. Defaults suit most games; a running session picks up what it can immediately, the rest applies on the next Play.")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			CrowdyStudioWidgets::Field(LOCTEXT("UdpProtoLabel", "UDP Protocol"),
				CrowdyStudioWidgets::SegmentedEnum(
					{ TEXT("Auto"), TEXT("IPv4"), TEXT("IPv6") },
					{ LOCTEXT("UdpAuto", "Auto"), LOCTEXT("UdpV4", "IPv4"), LOCTEXT("UdpV6", "IPv6") },
					TAttribute<FString>::CreateLambda([this]() { return UdpProtocolChoice; }),
					[this](const FString& V)
					{
						UdpProtocolChoice = V;
						const ECrowdyUDPProtocol P = (V == TEXT("IPv4")) ? ECrowdyUDPProtocol::IPv4
							: (V == TEXT("IPv6")) ? ECrowdyUDPProtocol::IPv6 : ECrowdyUDPProtocol::Auto;
						FCrowdyConfigSync::SetUdpProtocol(P);
						FCrowdyConfigSync::ApplyToRunningSessions();
					}),
				LOCTEXT("UdpProtoHint", "Auto tries IPv6 then falls back to IPv4. Force one only if your network needs it."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
		[
			CrowdyStudioWidgets::Field(LOCTEXT("UdpTimeoutLabel", "UDP Timeout (seconds)"),
				SNew(SBox).WidthOverride(160.0f).HAlign(HAlign_Left)
				[
					SNew(SSpinBox<float>)
					.MinValue(6.0f).MaxValue(120.0f).MinSliderValue(6.0f).MaxSliderValue(120.0f)
					.Delta(1.0f)
					.Value_Lambda([]() { return FCrowdyConfigSync::GetUdpTimeoutSeconds(); })
					.OnValueCommitted_Lambda([](float V, ETextCommit::Type)
					{
						FCrowdyConfigSync::SetUdpTimeoutSeconds(V);
						FCrowdyConfigSync::ApplyToRunningSessions();
					})
				],
				LOCTEXT("UdpTimeoutHint", "Silence before the connection is treated as dead and reconnects (6-120)."))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			CrowdyStudioWidgets::Field(LOCTEXT("HostPollLabel", "Host Poll Interval (seconds)"),
				SNew(SBox).WidthOverride(160.0f).HAlign(HAlign_Left)
				[
					SNew(SSpinBox<float>)
					.MinValue(1.0f).MaxValue(60.0f).MinSliderValue(1.0f).MaxSliderValue(60.0f)
					.Delta(1.0f)
					.Value_Lambda([]() { return FCrowdyConfigSync::GetHostPollIntervalSeconds(); })
					.OnValueCommitted_Lambda([](float V, ETextCommit::Type)
					{
						FCrowdyConfigSync::SetHostPollIntervalSeconds(V);
						FCrowdyConfigSync::ApplyToRunningSessions();
					})
				],
				LOCTEXT("HostPollHint", "How often the SDK checks the server for the current game host (1-60)."))
		],
		FMargin(16.0f, 14.0f));
}

void SCrowdyProjectView::RefreshVisibleApps()
{
	if (!Controller.IsValid())
	{
		return;
	}
	CrowdyFilterApps(Controller->GetApps(), Filter, VisibleApps);

	const TSharedPtr<FStudioApp> Selected = SelectedApp();
	if (AppListView.IsValid())
	{
		AppListView->RequestListRefresh();
		// Mirror the controller's selection without re-announcing it: a Direct select never reaches OnAppSelected.
		AppListView->ClearSelection();
		if (Selected.IsValid() && VisibleApps.Contains(Selected))
		{
			AppListView->SetSelection(Selected, ESelectInfo::Direct);
		}
	}
	if (DetailSwitcher.IsValid())
	{
		DetailSwitcher->SetActiveWidgetIndex(Selected.IsValid() ? 1 : 0);
	}
	// An app switch drops a half-finished edit; a refresh of the same app (discovery landing, a list refetch)
	// leaves the typed text alone.
	const int64 SelectedId = Selected.IsValid() ? Selected->AppId : 0;
	if (EditingAppId != 0 && EditingAppId != SelectedId && HeaderSwitcher.IsValid())
	{
		EditingAppId = 0;
		HeaderSwitcher->SetActiveWidgetIndex(0);
	}
}

void SCrowdyProjectView::HandleOrganizationsChanged()
{
	OrgOptions.Reset();
	TSharedPtr<FStudioOrg> All = MakeShared<FStudioOrg>();
	All->Name = TEXT("All organizations");
	OrgOptions.Add(All);
	if (Controller.IsValid())
	{
		OrgOptions.Append(Controller->GetOrganizations());
	}
	if (OrgComboBox.IsValid())
	{
		OrgComboBox->RefreshOptions();
	}
	RefreshVisibleApps();
}

void SCrowdyProjectView::HandleAppsChanged()
{
	RefreshVisibleApps();
}

TSharedRef<SWidget> SCrowdyProjectView::MakeOrgComboEntry(TSharedPtr<FStudioOrg> Org)
{
	const bool bAll = !Org.IsValid() || Org->OrgId == 0;
	const FString Label = bAll ? TEXT("All organizations") : FString::Printf(TEXT("%s  (%s)"), *Org->Name, *Org->Slug);
	return SNew(STextBlock).Text(FText::FromString(Label))
		.ColorAndOpacity(FSlateColor(bAll ? FCrowdyStudioStyle::TextSecondary() : FCrowdyStudioStyle::TextPrimary()));
}

void SCrowdyProjectView::OnOrgComboChanged(TSharedPtr<FStudioOrg> Org, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo == ESelectInfo::Direct || !Org.IsValid())
	{
		return;
	}
	// The filter is a view choice only. The controller's org follows the selected APP (SelectApp sets it), which is
	// what the permission gates and the persisted identity read; narrowing the list must not move that.
	Filter.OrgId = Org->OrgId;
	RefreshVisibleApps();
}

FText SCrowdyProjectView::GetSelectedOrgLabel() const
{
	if (Filter.OrgId == 0)
	{
		return LOCTEXT("AllOrgs", "All organizations");
	}
	for (const TSharedPtr<FStudioOrg>& Org : OrgOptions)
	{
		if (Org.IsValid() && Org->OrgId == Filter.OrgId)
		{
			return FText::FromString(FString::Printf(TEXT("%s  (%s)"), *Org->Name, *Org->Slug));
		}
	}
	return FText::Format(LOCTEXT("OrgById", "Organization #{0}"), FText::AsNumber(Filter.OrgId));
}

TSharedRef<ITableRow> SCrowdyProjectView::MakeAppRow(TSharedPtr<FStudioApp> App, const TSharedRef<STableViewBase>& OwnerTable)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	// Rows read the record live: the list reuses a row for the same pointer, and a single-app fetch merges in place.
	auto Get = [App](TFunction<FString(const FStudioApp&)> Field) { return [App, Field]() { return App.IsValid() ? Field(*App) : FString(); }; };
	auto Status = Get([](const FStudioApp& A) { return A.Status; });

	return SNew(STableRow<TSharedPtr<FStudioApp>>, OwnerTable)
		.Style(&Style, "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		.ToolTipText_Lambda([Get]()
		{
			return FText::FromString(Get([](const FStudioApp& A)
			{
				return FString::Printf(TEXT("%s\n#%lld  ·  %s"), *A.Name, A.AppId, *(A.OrgName.IsEmpty() ? FString::Printf(TEXT("org #%lld"), A.OrgId) : A.OrgName));
			})());
		})
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(9.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 9.0f, 0.0f)
				[ CrowdyStudioWidgets::Icon(TEXT("apps"), 15.0f, FSlateColor(FCrowdyStudioStyle::TextSubtle())) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.BodyStrong").OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.Text_Lambda([Get]() { return FText::FromString(Get([](const FStudioApp& A) { return A.Name; })()); }) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[ SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Mono", 8)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSubtle())).OverflowPolicy(ETextOverflowPolicy::Ellipsis)
						.Text_Lambda([Get]() { return FText::FromString(Get([](const FStudioApp& A) { return A.Slug; })()); }) ]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Pill")).Padding(FMargin(8.0f, 2.0f))
					.BorderBackgroundColor_Lambda([Status]() { FLinearColor C = CrowdyStudioWidgets::ColorForTone(CrowdyStudioWidgets::ToneForStatus(Status())); C.A = 0.16f; return FSlateColor(C); })
					[
						SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
						.Text_Lambda([Status]() { return FText::FromString(Status()); })
						.ColorAndOpacity_Lambda([Status]() { return FSlateColor(CrowdyStudioWidgets::ColorForTone(CrowdyStudioWidgets::ToneForStatus(Status()))); })
					]
				]
			]
		];
}

void SCrowdyProjectView::OnAppSelected(TSharedPtr<FStudioApp> App, ESelectInfo::Type SelectInfo)
{
	// A click or Enter selects; keyboard navigation through the rail does not, since each selection clears the
	// app token and app-scoped state and issues a fetch.
	if (SelectInfo != ESelectInfo::OnMouseClick && SelectInfo != ESelectInfo::OnKeyPress)
	{
		return;
	}
	if (Controller.IsValid() && App.IsValid())
	{
		Controller->SelectApp(App->AppId);
		// The controller announces the new app only once its fetch lands; swap the pane and drop any edit now.
		RefreshVisibleApps();
	}
}

FReply SCrowdyProjectView::OnCreateAppClicked()
{
	if (!Controller.IsValid() || !CreateDialog.IsValid())
	{
		return FReply::Handled();
	}
	const int64 OrgId = CreateOrgId();
	if (OrgId != 0)
	{
		CreateDialog->Open(OrgId);
	}
	return FReply::Handled();
}

int64 SCrowdyProjectView::CreateOrgId() const
{
	if (!Controller.IsValid())
	{
		return 0;
	}
	// The sheet needs one org the account is actually in: the filter's, else the selected app's (a remembered id
	// may belong to a previous account), else the only one there is.
	auto Known = [this](int64 Id)
	{
		for (const TSharedPtr<FStudioOrg>& Org : Controller->GetOrganizations())
		{
			if (Org.IsValid() && Org->OrgId == Id) { return true; }
		}
		return false;
	};
	if (Filter.OrgId != 0 && Known(Filter.OrgId)) { return Filter.OrgId; }
	if (Known(Controller->GetSelectedOrgId())) { return Controller->GetSelectedOrgId(); }
	return Controller->GetOrganizations().Num() == 1 ? Controller->GetOrganizations()[0]->OrgId : 0;
}

FReply SCrowdyProjectView::OnRefreshClicked()
{
	if (Controller.IsValid())
	{
		Controller->FetchMyOrganizations();
		Controller->FetchApps();
		if (const TSharedPtr<FStudioApp> A = SelectedApp())
		{
			Controller->FetchApp(A->AppId);
		}
	}
	return FReply::Handled();
}

FReply SCrowdyProjectView::OnSyncClicked()
{
	if (!Controller.IsValid())
	{
		return FReply::Handled();
	}
	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo,
		LOCTEXT("SyncConfirm", "Write the selected app's identifiers and endpoints into the project settings?"));
	if (Choice == EAppReturnType::Yes)
	{
		Controller->SyncConfig();
	}
	return FReply::Handled();
}

FReply SCrowdyProjectView::OnArchiveClicked()
{
	const TSharedPtr<FStudioApp> A = SelectedApp();
	if (!Controller.IsValid() || !A.IsValid())
	{
		return FReply::Handled();
	}
	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo,
		FText::Format(LOCTEXT("ArchiveConfirm", "Archive '{0}'? Players can no longer connect to it. You can set it back to Draft or Live later."), FText::FromString(A->Name)));
	if (Choice == EAppReturnType::Yes)
	{
		Controller->ArchiveApp(A->AppId);
	}
	return FReply::Handled();
}

FReply SCrowdyProjectView::OnSaveDetailsClicked()
{
	const TSharedPtr<FStudioApp> A = SelectedApp();
	// The boxes hold EditingAppId's text; if the selection moved underneath them, saving would rename the new app.
	if (!Controller.IsValid() || !A.IsValid() || A->AppId != EditingAppId)
	{
		return FReply::Handled();
	}
	const FString Name = EditNameBox.IsValid() ? EditNameBox->GetText().ToString().TrimStartAndEnd() : FString();
	const FString Description = EditDescriptionBox.IsValid() ? EditDescriptionBox->GetText().ToString().TrimStartAndEnd() : FString();
	if (Name.IsEmpty())
	{
		return FReply::Handled();
	}
	TOptional<FString> DescriptionChange;
	if (Description != A->Description)
	{
		DescriptionChange = Description;
	}
	Controller->UpdateApp(A->AppId, Name == A->Name ? FString() : Name, DescriptionChange, FString(), FString());
	EditingAppId = 0;
	if (HeaderSwitcher.IsValid()) { HeaderSwitcher->SetActiveWidgetIndex(0); }
	return FReply::Handled();
}

TSharedPtr<FStudioApp> SCrowdyProjectView::SelectedApp() const
{
	return Controller.IsValid() ? Controller->GetSelectedApp() : nullptr;
}

int32 SCrowdyProjectView::ConfigChangeCount() const
{
	if (!Controller.IsValid()) { return 0; }
	const FStudioSettingsSnapshot Cur = Controller->GetCurrentSettings();
	const FStudioSettingsSnapshot Prop = Controller->BuildProposedSettings();
	int32 N = 0;
	if (Cur.AppId != Prop.AppId) { ++N; }
	if (Cur.OrgId != Prop.OrgId) { ++N; }
	if (Cur.GameApiHttpUrl != Prop.GameApiHttpUrl) { ++N; }
	if (Cur.GameApiWsUrl != Prop.GameApiWsUrl) { ++N; }
	return N;
}

#undef LOCTEXT_NAMESPACE
