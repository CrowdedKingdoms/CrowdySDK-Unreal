// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyAccessView.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "Types/WidgetActiveTimerDelegate.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyAccessView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyAccessView::HandleAppChanged);
		Controller->OnFeaturesChanged.AddSP(this, &SCrowdyAccessView::HandleFeaturesChanged);
		Controller->OnTierFeaturesChanged.AddSP(this, &SCrowdyAccessView::HandleAccessListsChanged);
		Controller->OnAccessTiersChanged.AddSP(this, &SCrowdyAccessView::HandleAccessListsChanged);
	}

	// The page is built long after a remembered app was restored and selected, so without this the first list to
	// arrive reads as an app switch and empties editors the user may already have filled in.
	EditorAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;

	// Armed from the start so an app restored before this page was built is loaded the first time the page is shown.
	ScheduleEnsureLists();

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
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(LOCTEXT("AccessHeader", "Access")).TextStyle(&Style, "Crowdy.Text.Title") ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox).ToolTipText(LOCTEXT("AccessRefreshTip", "Re-read this app's features, access tiers and tier-feature grants from the server."))
				[ ToolbarButton(TEXT("refresh"), LOCTEXT("AccessRefresh", "Refresh"), FOnClicked::CreateSP(this, &SCrowdyAccessView::OnRefreshClicked), TEXT("Crowdy.Button.Ghost")) ]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SBox)
				.IsEnabled_Lambda([this]() { return CanEdit(); })
				.ToolTipText_Lambda([this]()
				{
					if (CanEdit()) { return LOCTEXT("DefineReady", "Define a new feature key for this app."); }
					if (Controller.IsValid() && !Controller->CanUseAppAccess()) { return LOCTEXT("DefineNeedsSession", "Features need a sign-in that can reach this app."); }
					return LOCTEXT("DefineNeedsApp", "Select an app you can manage on the Project page first.");
				})
				[ ToolbarButton(TEXT("plus"), LOCTEXT("DefineFeatureButton", "Define feature"), FOnClicked::CreateSP(this, &SCrowdyAccessView::OnDefineClicked), TEXT("Crowdy.Button.Secondary")) ]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true).Text(LOCTEXT("AccessSubtitle", "Feature keys for the selected app, and which access tiers grant them. Tiers themselves are created in the web console.")) ]

		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SAssignNew(BodySwitcher, SWidgetSwitcher)
			.WidgetIndex_Lambda([this]() { return Controller.IsValid() && Controller->GetSelectedAppId() != 0 ? 1 : 0; })
			+ SWidgetSwitcher::Slot()
			[
				CrowdyStudioWidgets::Card(
					CrowdyStudioWidgets::EmptyState(TEXT("key"), LOCTEXT("AccessNoApp", "Select an app on the Project page to see its features.")),
					FMargin(16.0f), /*bFlat*/ true)
			]
			+ SWidgetSwitcher::Slot()
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
	];

	RefreshVisibleFeatures();
}

SCrowdyAccessView::~SCrowdyAccessView()
{
	if (Controller.IsValid())
	{
		Controller->OnSelectedAppChanged.RemoveAll(this);
		Controller->OnFeaturesChanged.RemoveAll(this);
		Controller->OnTierFeaturesChanged.RemoveAll(this);
		Controller->OnAccessTiersChanged.RemoveAll(this);
	}
}

TSharedRef<SWidget> SCrowdyAccessView::BuildRail()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SAssignNew(SearchBox, SSearchBox)
			.Style(&Style, "Crowdy.SearchBox")
			.HintText(LOCTEXT("SearchFeatures", "Search features"))
			.DelayChangeNotificationsWhileTyping(true)
			.OnTextChanged_Lambda([this](const FText& T) { Search = T.ToString().TrimStartAndEnd(); RefreshVisibleFeatures(); })
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			CrowdyStudioWidgets::Card(
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SAssignNew(FeatureListView, SListView<TSharedPtr<FStudioAppFeature>>)
					.ListItemsSource(&VisibleFeatures)
					.OnGenerateRow(this, &SCrowdyAccessView::MakeFeatureRow)
					.OnSelectionChanged(this, &SCrowdyAccessView::OnFeatureSelected)
					.SelectionMode(ESelectionMode::Single)
				]
				// Empty and loading are different answers; only the first is a statement about the app.
				+ SOverlay::Slot()
				[
					SNew(SBox).Visibility_Lambda([this]()
					{
						return (Controller.IsValid() && VisibleFeatures.Num() == 0 && Controller->CanUseAppAccess() && !Controller->AreAccessListsLoading()) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						CrowdyStudioWidgets::EmptyState(TEXT("key"), LOCTEXT("NoFeatures", "No features match.\nClear the search, or define one."))
					]
				]
				+ SOverlay::Slot()
				[
					SNew(SBox).Visibility_Lambda([this]()
					{
						return (Controller.IsValid() && VisibleFeatures.Num() == 0 && !Controller->CanUseAppAccess()) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						CrowdyStudioWidgets::EmptyState(TEXT("key"), LOCTEXT("FeaturesNeedSession", "Features and grants need a sign-in that can reach this app."))
					]
				]
				+ SOverlay::Slot()
				[
					SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
					.Visibility_Lambda([this]()
					{
						return (Controller.IsValid() && VisibleFeatures.Num() == 0 && Controller->CanUseAppAccess() && Controller->AreAccessListsLoading()) ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
					})
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)[ SNew(SCircularThrobber).Radius(9.0f) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Text(LOCTEXT("LoadingFeatures", "Loading features...")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
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
				const int32 Total = Controller->GetFeatures().Num();
				const FText Count = VisibleFeatures.Num() == Total
					? FText::Format(LOCTEXT("FeatureCountAll", "{0} {0}|plural(one=feature,other=features)"), FText::AsNumber(Total))
					: FText::Format(LOCTEXT("FeatureCountSome", "{0} of {1} features"), FText::AsNumber(VisibleFeatures.Num()), FText::AsNumber(Total));
				const int32 Tiers = Controller->GetAccessTiers().Num();
				return FText::Format(LOCTEXT("FeatureCountTiers", "{0}  ·  {1} access {1}|plural(one=tier,other=tiers)"), Count, FText::AsNumber(Tiers));
			})
		];
}

TSharedRef<SWidget> SCrowdyAccessView::BuildDetail()
{
	return SAssignNew(DetailSwitcher, SWidgetSwitcher)
		+ SWidgetSwitcher::Slot()
		[
			CrowdyStudioWidgets::Card(
				CrowdyStudioWidgets::EmptyState(TEXT("key"), LOCTEXT("NoFeatureSelected", "Select a feature to see which access tiers grant it.")),
				FMargin(16.0f), /*bFlat*/ true)
		]
		+ SWidgetSwitcher::Slot()[ BuildDefineForm() ]
		+ SWidgetSwitcher::Slot()[ BuildFeatureDetail() ];
}

TSharedRef<SWidget> SCrowdyAccessView::BuildDefineForm()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			CrowdyStudioWidgets::Card(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
				[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Heading").Text(LOCTEXT("DefineHeading", "Define a feature")) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
				[
					CrowdyStudioWidgets::Field(LOCTEXT("FeatureKeyLabel", "Feature key"),
						SAssignNew(FeatureKeyBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("FeatureKeyHint", "premium_maps"))
							.IsEnabled_Lambda([this]() { return !Controller.IsValid() || !Controller->IsAccessWriteInFlight(); }),
						LOCTEXT("FeatureKeyFieldHint", "The key server logic checks a player's tier for. Defining an existing key updates its description."))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
				[
					CrowdyStudioWidgets::Field(LOCTEXT("FeatureDescLabel", "Description"),
						SAssignNew(FeatureDescBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("FeatureDescHint", "Optional"))
							.IsEnabled_Lambda([this]() { return !Controller.IsValid() || !Controller->IsAccessWriteInFlight(); }))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Ghost").ContentPadding(FMargin(11.0f, 6.0f))
						.OnClicked(this, &SCrowdyAccessView::OnCancelDefineClicked)
						[ SNew(STextBlock).Text(LOCTEXT("CancelDefine", "Cancel")).TextStyle(&Style, "Crowdy.Text.Body") ]
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(11.0f, 6.0f))
						.IsEnabled_Lambda([this]() { return CanEdit() && !IsAccessBusy() && FeatureKeyBox.IsValid() && !FeatureKeyBox->GetText().ToString().TrimStartAndEnd().IsEmpty(); })
						.OnClicked(this, &SCrowdyAccessView::OnSaveFeatureClicked)
						[ SNew(STextBlock).Text(LOCTEXT("SaveFeature", "Save")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
					]
				],
				FMargin(16.0f, 14.0f))
		];
}

TSharedRef<SWidget> SCrowdyAccessView::BuildFeatureDetail()
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	auto Key = [this]() { return SelectedFeatureKey; };
	auto Description = [this]() { const TSharedPtr<FStudioAppFeature> F = SelectedFeature(); return F.IsValid() ? F->Description : FString(); };

	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Heading").OverflowPolicy(ETextOverflowPolicy::Ellipsis).Text_Lambda([Key]() { return FText::FromString(Key()); }) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ CrowdyStudioWidgets::CopyChip(TAttribute<FString>::CreateLambda(Key), LOCTEXT("FeatureKeyWhat", "feature key")) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body")
						.ColorAndOpacity_Lambda([Description]() { return FSlateColor(Description().IsEmpty() ? FCrowdyStudioStyle::TextSubtle() : FCrowdyStudioStyle::TextPrimary()); })
						.Text_Lambda([Description]() { const FString D = Description(); return D.IsEmpty() ? LOCTEXT("NoDescription", "No description.") : FText::FromString(D); })
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true)
						.Text_Lambda([this, Key]()
						{
							const int32 N = GrantCount(Key());
							return N == 0 ? LOCTEXT("GrantedByNone", "No access tier grants this feature yet.")
								: FText::Format(LOCTEXT("GrantedBySome", "Granted by {0} access {0}|plural(one=tier,other=tiers)."), FText::AsNumber(N));
						})
					],
					FMargin(14.0f, 12.0f))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("AccessTiers", "Access tiers"), TEXT("users")) ]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						[
							SAssignNew(TierListView, SListView<TSharedPtr<FStudioAccessTier>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetAccessTiers() : nullptr)
							.OnGenerateRow(this, &SCrowdyAccessView::MakeTierRow)
							.SelectionMode(ESelectionMode::None)
						]
						+ SOverlay::Slot()
						[
							SNew(SBox).Visibility_Lambda([this]()
							{
								return Controller.IsValid() && Controller->GetAccessTiers().Num() == 0 && !Controller->AreAccessListsLoading() ? EVisibility::Visible : EVisibility::Collapsed;
							})
							[ CrowdyStudioWidgets::EmptyState(TEXT("users"), LOCTEXT("NoTiers", "This app has no access tiers.\nCreate them in the web console.")) ]
						]
					],
					FMargin(16.0f, 14.0f))
			]
		];
}

TSharedRef<ITableRow> SCrowdyAccessView::MakeFeatureRow(TSharedPtr<FStudioAppFeature> Feature, const TSharedRef<STableViewBase>& OwnerTable)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const FString Key = Feature.IsValid() ? Feature->FeatureKey : FString();
	const FString Desc = Feature.IsValid() ? Feature->Description : FString();

	return SNew(STableRow<TSharedPtr<FStudioAppFeature>>, OwnerTable)
		.Style(&Style, "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		.ToolTipText(FText::FromString(Desc.IsEmpty() ? Key : Key + TEXT("\n") + Desc))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(9.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 9.0f, 0.0f)
				[ CrowdyStudioWidgets::Icon(TEXT("key"), 15.0f, FSlateColor(FCrowdyStudioStyle::TextSubtle())) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.BodyStrong").OverflowPolicy(ETextOverflowPolicy::Ellipsis).Text(FText::FromString(Key)) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").OverflowPolicy(ETextOverflowPolicy::Ellipsis).Text(FText::FromString(Desc))
						.Visibility(Desc.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary()))
					.Text_Lambda([this, Key]()
					{
						const int32 N = GrantCount(Key);
						return N == 0 ? LOCTEXT("NoTierPill", "NO TIERS") : FText::Format(LOCTEXT("TierPill", "{0} {0}|plural(one=TIER,other=TIERS)"), FText::AsNumber(N));
					})
				]
			]
		];
}

TSharedRef<ITableRow> SCrowdyAccessView::MakeTierRow(TSharedPtr<FStudioAccessTier> Tier, const TSharedRef<STableViewBase>& OwnerTable)
{
	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const int64 TierId = Tier.IsValid() ? Tier->TierId : 0;
	auto Granted = [this, TierId]() { return IsGranted(TierId, SelectedFeatureKey); };

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.BodyStrong").Text(FText::FromString(Tier.IsValid() ? Tier->Name : FString())) ]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
		[ SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Mono", 8)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSubtle())).Text(FText::FromString(FString::Printf(TEXT("#%lld"), TierId))) ];

	if (Tier.IsValid() && Tier->bIsDefault)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)[ CrowdyStudioWidgets::Badge(LOCTEXT("DefaultTier", "DEFAULT"), CrowdyStudioWidgets::EBadgeTone::Info) ];
	}
	if (Tier.IsValid() && Tier->bIsFree)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 4.0f, 0.0f)[ CrowdyStudioWidgets::Badge(LOCTEXT("FreeTier", "FREE"), CrowdyStudioWidgets::EBadgeTone::Neutral) ];
	}

	Row->AddSlot().FillWidth(1.0f)[ SNullWidget::NullWidget ];
	Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
	[
		SNew(SBox).Visibility_Lambda([Granted]() { return Granted() ? EVisibility::Visible : EVisibility::Collapsed; })
		[ CrowdyStudioWidgets::Badge(LOCTEXT("GrantedBadge", "GRANTED"), CrowdyStudioWidgets::EBadgeTone::Success) ]
	];
	Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(11.0f, 5.0f))
		.IsEnabled_Lambda([this]() { return CanEdit() && !SelectedFeatureKey.IsEmpty() && !IsAccessBusy(); })
		.ToolTipText_Lambda([Granted]()
		{
			return Granted() ? LOCTEXT("RevokeTip", "Players on this tier lose the feature.") : LOCTEXT("GrantTip", "Players on this tier get the feature.");
		})
		.OnClicked_Lambda([this, TierId, Granted]()
		{
			if (!Controller.IsValid() || SelectedFeatureKey.IsEmpty() || IsAccessBusy()) { return FReply::Handled(); }
			if (Granted()) { Controller->RevokeTierFeature(TierId, SelectedFeatureKey); }
			else { Controller->GrantTierFeature(TierId, SelectedFeatureKey); }
			return FReply::Handled();
		})
		[
			SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground())
			.Text_Lambda([Granted]() { return Granted() ? LOCTEXT("RevokeTierFeature", "Revoke") : LOCTEXT("GrantTierFeature", "Grant"); })
		]
	];

	return SNew(STableRow<TSharedPtr<FStudioAccessTier>>, OwnerTable)
		.Style(&Style, "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[ SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))[ Row ] ];
}

FReply SCrowdyAccessView::OnRefreshClicked()
{
	if (Controller.IsValid())
	{
		Controller->FetchFeatures();
		Controller->FetchTierFeatures();
		Controller->FetchAppAccessTiers();
	}
	return FReply::Handled();
}

FReply SCrowdyAccessView::OnDefineClicked()
{
	if (FeatureKeyBox.IsValid()) { FeatureKeyBox->SetText(FText::GetEmpty()); }
	if (FeatureDescBox.IsValid()) { FeatureDescBox->SetText(FText::GetEmpty()); }
	bDefining = true;
	// The kept key stays; only the list's highlight goes, so clicking that same row afterwards is a real selection change.
	if (FeatureListView.IsValid()) { FeatureListView->ClearSelection(); }
	ShowDetail();
	return FReply::Handled();
}

FReply SCrowdyAccessView::OnCancelDefineClicked()
{
	bDefining = false;
	PendingSelectKey.Reset();
	RefreshVisibleFeatures();
	return FReply::Handled();
}

FReply SCrowdyAccessView::OnSaveFeatureClicked()
{
	if (!Controller.IsValid() || !FeatureKeyBox.IsValid() || !FeatureDescBox.IsValid() || IsAccessBusy())
	{
		return FReply::Handled();
	}
	const FString Key = FeatureKeyBox->GetText().ToString().TrimStartAndEnd();
	Controller->DefineFeature(Key, FeatureDescBox->GetText().ToString().TrimStartAndEnd());
	// The form stays open: it closes when the re-read brings the key in, so a failed define keeps what was typed.
	PendingSelectKey = Key;
	return FReply::Handled();
}

void SCrowdyAccessView::ScheduleEnsureLists()
{
	if (EnsureListsTimerHandle.IsValid())
	{
		return;
	}

	EnsureListsTimerHandle = RegisterActiveTimer(0.0f,
		FWidgetActiveTimerDelegate::CreateSP(this, &SCrowdyAccessView::HandleEnsureLists));
}

EActiveTimerReturnType SCrowdyAccessView::HandleEnsureLists(double InCurrentTime, float InDeltaTime)
{
	EnsureListsTimerHandle.Reset();

	if (Controller.IsValid())
	{
		// The controller decides whether anything is owed: it knows which app the lists were last requested for.
		Controller->EnsureAccessListsLoaded();
	}

	return EActiveTimerReturnType::Stop;
}

void SCrowdyAccessView::HandleAppChanged()
{
	SyncEditorAppScope();
	ScheduleEnsureLists();
}

void SCrowdyAccessView::SyncEditorAppScope()
{
	const int64 CurrentAppId = Controller.IsValid() ? Controller->GetSelectedAppId() : 0;
	if (CurrentAppId == EditorAppId)
	{
		return;
	}

	EditorAppId = CurrentAppId;
	ClearAppScopedEditors();
}

void SCrowdyAccessView::ClearAppScopedEditors()
{
	// A key left in the form or the selection would let Save, Grant or Revoke act on the new app while describing a
	// feature read from the old one.
	if (FeatureKeyBox.IsValid()) { FeatureKeyBox->SetText(FText::GetEmpty()); }
	if (FeatureDescBox.IsValid()) { FeatureDescBox->SetText(FText::GetEmpty()); }
	SelectedFeatureKey.Reset();
	PendingSelectKey.Reset();
	bDefining = false;
	RefreshVisibleFeatures();
}

void SCrowdyAccessView::HandleFeaturesChanged()
{
	SyncEditorAppScope();
	// A read that lands while the define is still awaiting its reply predates it, so it cannot settle the form.
	if (!PendingSelectKey.IsEmpty() && !Controller->IsAccessWriteInFlight())
	{
		for (const TSharedPtr<FStudioAppFeature>& Feature : Controller->GetFeatures())
		{
			if (Feature.IsValid() && Feature->FeatureKey.Equals(PendingSelectKey, ESearchCase::CaseSensitive))
			{
				SelectedFeatureKey = PendingSelectKey;
				PendingSelectKey.Reset();
				bDefining = false;
				break;
			}
		}
	}
	RefreshVisibleFeatures();
}

void SCrowdyAccessView::HandleAccessListsChanged()
{
	SyncEditorAppScope();
	if (TierListView.IsValid())
	{
		TierListView->RequestListRefresh();
	}
}

void SCrowdyAccessView::RefreshVisibleFeatures()
{
	VisibleFeatures.Reset();
	if (Controller.IsValid())
	{
		for (const TSharedPtr<FStudioAppFeature>& Feature : Controller->GetFeatures())
		{
			if (Feature.IsValid() && (Search.IsEmpty() || Feature->FeatureKey.Contains(Search) || Feature->Description.Contains(Search)))
			{
				VisibleFeatures.Add(Feature);
			}
		}
	}

	if (FeatureListView.IsValid())
	{
		FeatureListView->RequestListRefresh();
		// Mirror the kept key without re-announcing it: a Direct select never reaches OnFeatureSelected.
		FeatureListView->ClearSelection();
		const TSharedPtr<FStudioAppFeature> Selected = SelectedFeature();
		if (Selected.IsValid() && VisibleFeatures.Contains(Selected))
		{
			FeatureListView->SetSelection(Selected, ESelectInfo::Direct);
		}
	}
	ShowDetail();
}

void SCrowdyAccessView::ShowDetail()
{
	if (!DetailSwitcher.IsValid())
	{
		return;
	}
	const int32 Index = bDefining ? 1 : (SelectedFeature().IsValid() ? 2 : 0);
	DetailSwitcher->SetActiveWidgetIndex(Index);
	if (Index == 2 && TierListView.IsValid())
	{
		TierListView->RequestListRefresh();
	}
}

void SCrowdyAccessView::OnFeatureSelected(TSharedPtr<FStudioAppFeature> Feature, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo == ESelectInfo::Direct || !Feature.IsValid())
	{
		return;
	}
	SelectedFeatureKey = Feature->FeatureKey;
	PendingSelectKey.Reset();
	bDefining = false;
	ShowDetail();
}

TSharedPtr<FStudioAppFeature> SCrowdyAccessView::SelectedFeature() const
{
	if (!Controller.IsValid() || SelectedFeatureKey.IsEmpty())
	{
		return nullptr;
	}
	for (const TSharedPtr<FStudioAppFeature>& Feature : Controller->GetFeatures())
	{
		if (Feature.IsValid() && Feature->FeatureKey.Equals(SelectedFeatureKey, ESearchCase::CaseSensitive))
		{
			return Feature;
		}
	}
	return nullptr;
}

bool SCrowdyAccessView::IsGranted(int64 TierId, const FString& FeatureKey) const
{
	if (!Controller.IsValid())
	{
		return false;
	}
	for (const TSharedPtr<FStudioTierFeature>& Grant : Controller->GetTierFeatures())
	{
		if (Grant.IsValid() && Grant->TierId == TierId && Grant->FeatureKey.Equals(FeatureKey, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

int32 SCrowdyAccessView::GrantCount(const FString& FeatureKey) const
{
	int32 Count = 0;
	if (!Controller.IsValid())
	{
		return Count;
	}
	for (const TSharedPtr<FStudioTierFeature>& Grant : Controller->GetTierFeatures())
	{
		if (Grant.IsValid() && Grant->FeatureKey.Equals(FeatureKey, ESearchCase::CaseSensitive))
		{
			++Count;
		}
	}
	return Count;
}

bool SCrowdyAccessView::CanEdit() const
{
	return Controller.IsValid() && Controller->GetSelectedAppId() != 0 && Controller->CanManageApps() && Controller->CanUseAppAccess();
}

bool SCrowdyAccessView::IsAccessBusy() const
{
	return Controller.IsValid() && (Controller->IsAccessWriteInFlight() || Controller->AreAccessListsLoading());
}

#undef LOCTEXT_NAMESPACE
