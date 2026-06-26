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
#include "UI/SCrowdyBackendSelector.h"
#include "UI/SCrowdyEnvironmentsView.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyProjectView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnOrganizationsChanged.AddSP(this, &SCrowdyProjectView::HandleOrganizationsChanged);
		Controller->OnAppsChanged.AddSP(this, &SCrowdyProjectView::HandleAppsChanged);
	}

	// Seed the Connection section's protocol toggle from the persisted setting.
	switch (FCrowdyConfigSync::GetUdpProtocol())
	{
	case ECrowdyUDPProtocol::IPv4: UdpProtocolChoice = TEXT("IPv4"); break;
	case ECrowdyUDPProtocol::IPv6: UdpProtocolChoice = TEXT("IPv6"); break;
	default:                       UdpProtocolChoice = TEXT("Auto"); break;
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// How many project settings the selected app would change (drives the summary chip).
	auto ChangeCount = [this]() -> int32
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
	};

	// One config setting as a readable row: label on the left, the resulting value on the right
	// (gold when it will change), with a dimmed "was …" line shown only when it actually differs.
	auto MakeSettingRow = [](const FText& Label, TFunction<FString()> GetCurrent, TFunction<FString()> GetProposed) -> TSharedRef<SWidget>
	{
		const ISlateStyle& S = FCrowdyStudioStyle::Get();
		const FSlateFontInfo Mono = FCoreStyle::GetDefaultFontStyle("Mono", 9);
		auto Changed = [GetCurrent, GetProposed]() { return GetCurrent() != GetProposed(); };

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 1.0f, 18.0f, 0.0f)
			[ SNew(SBox).WidthOverride(150.0f)[ SNew(STextBlock).Text(Label).AutoWrapText(true).TextStyle(&S, "Crowdy.Text.Body") ] ]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Font(Mono).AutoWrapText(true)
					.Text_Lambda([GetProposed]() { return FText::FromString(GetProposed()); })
					.ColorAndOpacity_Lambda([Changed]() { return Changed() ? FSlateColor(FCrowdyStudioStyle::GoldBright()) : FSlateColor(FCrowdyStudioStyle::TextSecondary()); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox).Visibility_Lambda([Changed]() { return Changed() ? EVisibility::Visible : EVisibility::Collapsed; })
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[ SNew(STextBlock).Text(LOCTEXT("WasLabel", "was")).TextStyle(&S, "Crowdy.Text.Subtle") ]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[ SNew(STextBlock).Font(Mono).AutoWrapText(true).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSubtle())).Text_Lambda([GetCurrent]() { return FText::FromString(GetCurrent()); }) ]
				]
			];
	};

	auto Divider = [&Style]() -> TSharedRef<SWidget>
	{
		return SNew(SBox).HeightOverride(1.0f)[ SNew(SImage).Image(Style.GetBrush("Crowdy.Separator")) ];
	};

	const TSharedRef<SVerticalBox> SettingsList = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[ MakeSettingRow(LOCTEXT("RowAppId", "App ID"),
			[this]() { return FString::Printf(TEXT("%lld"), Controller->GetCurrentSettings().AppId); },
			[this]() { return FString::Printf(TEXT("%lld"), Controller->BuildProposedSettings().AppId); }) ]
		+ SVerticalBox::Slot().AutoHeight()[ Divider() ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f)
		[ MakeSettingRow(LOCTEXT("RowOrgId", "Org ID"),
			[this]() { return FString::Printf(TEXT("%d"), Controller->GetCurrentSettings().OrgId); },
			[this]() { return FString::Printf(TEXT("%d"), Controller->BuildProposedSettings().OrgId); }) ]
		+ SVerticalBox::Slot().AutoHeight()[ Divider() ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f)
		[ MakeSettingRow(LOCTEXT("RowGameHttp", "Game API HTTP URL"),
			[this]() { return Controller->GetCurrentSettings().GameApiHttpUrl; },
			[this]() { return Controller->BuildProposedSettings().GameApiHttpUrl; }) ]
		+ SVerticalBox::Slot().AutoHeight()[ Divider() ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 10.0f, 0.0f, 0.0f)
		[ MakeSettingRow(LOCTEXT("RowGameWs", "Game API WS URL"),
			[this]() { return Controller->GetCurrentSettings().GameApiWsUrl; },
			[this]() { return Controller->BuildProposedSettings().GameApiWsUrl; }) ];

	// Summary chip for the section header: "In sync" or "N to change".
	const TSharedRef<SWidget> ChangeBadge = SNew(SBorder)
		.BorderImage(Style.GetBrush("Crowdy.Pill"))
		.BorderBackgroundColor_Lambda([ChangeCount]()
		{
			FLinearColor C = ChangeCount() > 0 ? FCrowdyStudioStyle::Gold() : FCrowdyStudioStyle::Success();
			C.A = 0.16f;
			return FSlateColor(C);
		})
		.Padding(FMargin(8.0f, 2.0f))
		[
			SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
			.Text_Lambda([ChangeCount]()
			{
				const int32 N = ChangeCount();
				return N > 0 ? FText::Format(LOCTEXT("NChanges", "{0} TO CHANGE"), FText::AsNumber(N)) : LOCTEXT("InSync", "IN SYNC");
			})
			.ColorAndOpacity_Lambda([ChangeCount]() { return FSlateColor(ChangeCount() > 0 ? FCrowdyStudioStyle::GoldBright() : FCrowdyStudioStyle::Success()); })
		];

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot().Padding(2.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[ SNew(STextBlock).Text(LOCTEXT("ProjectHeader", "Project")).TextStyle(&Style, "Crowdy.Text.Title") ]

			// Backend: which Crowdy management plane to talk to. Also on the Sign In page, so a fresh
			// project can choose a backend before signing in.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[ SNew(SCrowdyBackendSelector).Controller(Controller) ]

			// Organization picker.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[ SNew(STextBlock).Text(LOCTEXT("OrgPickerLabel", "Organization")).TextStyle(&Style, "Crowdy.Text.BodyStrong") ]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SAssignNew(OrgComboBox, SComboBox<TSharedPtr<FStudioOrg>>)
					.OptionsSource(Controller.IsValid() ? &Controller->GetOrganizations() : nullptr)
					.OnGenerateWidget(this, &SCrowdyProjectView::MakeOrgComboEntry)
					.OnSelectionChanged(this, &SCrowdyProjectView::OnOrgComboChanged)
					[ SNew(STextBlock).Text(this, &SCrowdyProjectView::GetSelectedOrgLabel).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextPrimary())) ]
				]
			]

			// App list.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				SNew(SBox).HeightOverride(230.0f)
				[
					CrowdyStudioWidgets::Card(
						SNew(SOverlay)
						+ SOverlay::Slot()
						[
							SAssignNew(AppListView, SListView<TSharedPtr<FStudioApp>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetApps() : nullptr)
							.OnGenerateRow(this, &SCrowdyProjectView::MakeAppRow)
							.OnSelectionChanged(this, &SCrowdyProjectView::OnAppSelected)
							.SelectionMode(ESelectionMode::Single)
						]
						+ SOverlay::Slot()
						[
							SNew(SBox).Visibility_Lambda([this]() { return (Controller.IsValid() && Controller->GetApps().Num() == 0) ? EVisibility::Visible : EVisibility::Collapsed; })
							[ CrowdyStudioWidgets::EmptyState(TEXT("apps"), LOCTEXT("NoApps", "No apps yet.\nCreate one below, or sign in to load your apps.")) ]
						],
						FMargin(6.0f), /*bFlat*/ true)
				]
			]

			// Create app.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("CreateAppHeader", "Create app"), TEXT("plus")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[ CrowdyStudioWidgets::Field(LOCTEXT("AppNameLabel", "Name"), SAssignNew(NameBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("AppNameHint", "My Game"))) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[ CrowdyStudioWidgets::Field(LOCTEXT("AppSlugLabel", "Slug"), SAssignNew(SlugBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("AppSlugHint", "my-game"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							CrowdyStudioWidgets::Field(LOCTEXT("AppStatusLabel", "Status"),
								CrowdyStudioWidgets::SegmentedEnum(
									{ TEXT("DRAFT"), TEXT("LIVE"), TEXT("ARCHIVED") },
									{ LOCTEXT("StDraft", "Draft"), LOCTEXT("StLive", "Live"), LOCTEXT("StArchived", "Archived") },
									TAttribute<FString>::CreateLambda([this]() { return CreateStatus; }),
									[this](const FString& V) { CreateStatus = V; }))
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[
							CrowdyStudioWidgets::Field(LOCTEXT("AppVisLabel", "Visibility"),
								CrowdyStudioWidgets::SegmentedEnum(
									{ TEXT("PRIVATE"), TEXT("UNLISTED"), TEXT("PUBLIC") },
									{ LOCTEXT("VisPrivate", "Private"), LOCTEXT("VisUnlisted", "Unlisted"), LOCTEXT("VisPublic", "Public") },
									TAttribute<FString>::CreateLambda([this]() { return CreateVisibility; }),
									[this](const FString& V) { CreateVisibility = V; }))
						]
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[
						SNew(SButton)
						.ButtonStyle(&Style, "Crowdy.Button.Secondary")
						.ContentPadding(FMargin(13.0f, 7.0f))
						.IsEnabled_Lambda([this]() { return Controller.IsValid() && Controller->CanManageApps(); })
						.ToolTipText_Lambda([this]()
						{
							if (!Controller.IsValid() || Controller->GetSelectedOrgId() == 0) { return LOCTEXT("CreateAppNeedsOrg", "Select an organization first."); }
							if (!Controller->CanManageApps()) { return LOCTEXT("CreateAppNoPerm", "Your role in this organization can't create apps (needs the manage_apps permission)."); }
							return LOCTEXT("CreateAppReady", "Create a new app in the selected organization.");
						})
						.OnClicked(this, &SCrowdyProjectView::OnCreateAppClicked)
						[ SNew(STextBlock).Text(LOCTEXT("CreateAppButton", "Create App")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Project configuration (the former Config Sync page, folded in here).
			+ SVerticalBox::Slot().AutoHeight()
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("ConfigHeader", "Project configuration"), TEXT("config"), ChangeBadge) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
					[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body").Text(LOCTEXT("ConfigExplainer", "What the selected app writes into the project (DefaultGame.ini): its app id, org, and the game endpoints from the app's own routing. The Management URL comes from the Backend section above. Changed values are gold; the runtime picks them up on the next PIE or launch.")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
					[
						SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(14.0f, 12.0f))
						[ SettingsList ]
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)[ SNullWidget::NullWidget ]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.ButtonStyle(&Style, "Crowdy.Button.Primary")
							.ContentPadding(FMargin(14.0f, 8.0f))
							.IsEnabled_Lambda([this]() { return Controller.IsValid() && Controller->GetSelectedApp().IsValid(); })
							.OnClicked(this, &SCrowdyProjectView::OnSyncClicked)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)[ CrowdyStudioWidgets::Icon(TEXT("check"), 15.0f, FSlateColor::UseForeground()) ]
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ SNew(STextBlock).Text(LOCTEXT("SyncToProjectButton", "Sync to Project")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
							]
						]
					],
					FMargin(16.0f, 14.0f))
			]

			// Connection (advanced): the realtime UDP knobs, edited here so the console is the one
				// place for network config. Collapsed by default since defaults suit most games.
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(SExpandableArea)
					.InitiallyCollapsed(true)
					.HeaderContent()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[ CrowdyStudioWidgets::Icon(TEXT("broadcast"), 15.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[ SNew(STextBlock).Text(LOCTEXT("ConnHeader", "Connection (advanced)")).TextStyle(&Style, "Crowdy.Text.BodyStrong") ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
						[ SNew(STextBlock).Text(LOCTEXT("ConnHint", "UDP connection tuning. Defaults suit most games.")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
					]
					.BodyContent()
					[
						SNew(SBox).Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
						[
							CrowdyStudioWidgets::Card(
								SNew(SVerticalBox)
								+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 14.0f)
								[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body").Text(LOCTEXT("ConnExplainer", "These tune the realtime UDP connection. They are written to DefaultGame.ini and apply on the next Play (a running session picks up what it can immediately).")) ]

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
								FMargin(16.0f, 14.0f))
						]
					]
				]

				// Game server: where this app runs. Folded in from the old Environments page and
			// collapsed by default, since most projects just use the default server.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.OnAreaExpansionChanged_Lambda([this](bool bExpanded)
				{
					// Lazy-load the org's environments the first time the section opens, so the list
					// is there without a manual Refresh. The embedded view's Refresh button still
					// re-pulls on demand.
					if (bExpanded && Controller.IsValid() && Controller->GetSelectedOrgId() != 0
						&& Controller->GetEnvironments().Num() == 0)
					{
						Controller->FetchEnvironments(Controller->GetSelectedOrgId());
					}
				})
				.HeaderContent()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[ CrowdyStudioWidgets::Icon(TEXT("server"), 15.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[ SNew(STextBlock).Text(LOCTEXT("GameServerHeader", "Game server")).TextStyle(&Style, "Crowdy.Text.BodyStrong") ]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
					[ SNew(STextBlock).Text(LOCTEXT("GameServerHint", "Advanced. Most projects use the default server.")).TextStyle(&Style, "Crowdy.Text.Subtle") ]
				]
				.BodyContent()
				[
					SNew(SBox).Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
					[ SNew(SCrowdyEnvironmentsView).Controller(Controller).Embedded(true) ]
				]
			]
		]
	];
}

SCrowdyProjectView::~SCrowdyProjectView()
{
	if (Controller.IsValid())
	{
		Controller->OnOrganizationsChanged.RemoveAll(this);
		Controller->OnAppsChanged.RemoveAll(this);
	}
}

FReply SCrowdyProjectView::OnCreateAppClicked()
{
	if (Controller.IsValid() && NameBox.IsValid() && SlugBox.IsValid())
	{
		Controller->CreateApp(Controller->GetSelectedOrgId(), NameBox->GetText().ToString(), SlugBox->GetText().ToString(), CreateStatus, CreateVisibility);
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

void SCrowdyProjectView::HandleOrganizationsChanged()
{
	if (OrgComboBox.IsValid())
	{
		OrgComboBox->RefreshOptions();
	}
}

void SCrowdyProjectView::HandleAppsChanged()
{
	if (AppListView.IsValid())
	{
		AppListView->RequestListRefresh();
	}
}

TSharedRef<SWidget> SCrowdyProjectView::MakeOrgComboEntry(TSharedPtr<FStudioOrg> Org)
{
	const FString Label = Org.IsValid() ? FString::Printf(TEXT("%s  (%s)"), *Org->Name, *Org->Slug) : FString();
	return SNew(STextBlock).Text(FText::FromString(Label));
}

void SCrowdyProjectView::OnOrgComboChanged(TSharedPtr<FStudioOrg> Org, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo != ESelectInfo::Direct && Controller.IsValid() && Org.IsValid())
	{
		Controller->SelectOrg(Org->OrgId);
	}
}

FText SCrowdyProjectView::GetSelectedOrgLabel() const
{
	if (Controller.IsValid())
	{
		if (const TSharedPtr<FStudioOrg> Org = Controller->GetSelectedOrg())
		{
			return FText::FromString(FString::Printf(TEXT("%s  (%s)"), *Org->Name, *Org->Slug));
		}
	}
	return LOCTEXT("PickOrg", "Select an organization");
}

TSharedRef<ITableRow> SCrowdyProjectView::MakeAppRow(TSharedPtr<FStudioApp> App, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Name = App.IsValid() ? App->Name : FString();
	const FString Slug = App.IsValid() ? App->Slug : FString();
	const FString Id = App.IsValid() ? FString::Printf(TEXT("#%lld"), App->AppId) : FString();
	const FString Status = App.IsValid() ? App->Status : FString();

	return SNew(STableRow<TSharedPtr<FStudioApp>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(9.0f, 8.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 11.0f, 0.0f)
			[
				SNew(SBorder).BorderImage(FCrowdyStudioStyle::Get().GetBrush("Crowdy.Inset")).Padding(FMargin(5.0f))
				[ CrowdyStudioWidgets::Icon(TEXT("apps"), 16.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()[ SNew(STextBlock).Text(FText::FromString(Name)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ CrowdyStudioWidgets::Chip(FText::FromString(Slug)) ]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)[ SNew(STextBlock).Text(FText::FromString(Id)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ CrowdyStudioWidgets::Badge(FText::FromString(Status), CrowdyStudioWidgets::ToneForStatus(Status)) ]
		];
}

void SCrowdyProjectView::OnAppSelected(TSharedPtr<FStudioApp> App, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo != ESelectInfo::Direct && Controller.IsValid() && App.IsValid())
	{
		Controller->SelectApp(App->AppId);
	}
}

#undef LOCTEXT_NAMESPACE
