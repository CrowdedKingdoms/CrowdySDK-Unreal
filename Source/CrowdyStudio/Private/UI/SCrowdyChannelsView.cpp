// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyChannelsView.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/SCrowdyGroupDetailView.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyChannelsView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnChannelsChanged.AddSP(this, &SCrowdyChannelsView::HandleChannelsChanged);
		Controller->OnChannelPolicyChanged.AddSP(this, &SCrowdyChannelsView::HandlePolicyChanged);
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyChannelsView::HandleAppChanged);
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> RefreshButton = SNew(SButton)
		.ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(9.0f, 5.0f))
		.OnClicked(this, &SCrowdyChannelsView::OnRefreshClicked)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ CrowdyStudioWidgets::Icon(TEXT("refresh"), 14.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(LOCTEXT("ChannelsRefresh", "Refresh")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
		];

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[ SNew(STextBlock).Text(LOCTEXT("ChannelsHeader", "Channels")).TextStyle(&Style, "Crowdy.Text.Title") ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("ChannelsPlane", "Channels carry distance-independent messages (chat, the Reliable-RPC session channel). They live on the game server, so sign in with your email and password for these to load (an org token only covers account settings).")) ]

			// Channel list.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("ChannelsList", "Channels"), TEXT("broadcast"), RefreshButton) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SBox).MinDesiredHeight(120.0f).MaxDesiredHeight(260.0f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						[
							SAssignNew(ChannelListView, SListView<TSharedPtr<FStudioGroup>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetChannels() : nullptr)
							.OnGenerateRow(this, &SCrowdyChannelsView::MakeChannelRow)
							.OnSelectionChanged(this, &SCrowdyChannelsView::OnChannelSelected)
							.SelectionMode(ESelectionMode::Single)
						]
						+ SOverlay::Slot()
						[
							SNew(SBox)
							.Visibility_Lambda([this]() { return (Controller.IsValid() && Controller->GetChannels().Num() == 0) ? EVisibility::Visible : EVisibility::Collapsed; })
							[ CrowdyStudioWidgets::EmptyState(TEXT("broadcast"), LOCTEXT("NoChannels", "No channels loaded.\nPress Refresh after signing in.")) ]
						]
					],
					FMargin(6.0f), /*bFlat*/ true)
			]

			// Drill-in detail (members + roles of the selected channel).
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[ SNew(SCrowdyGroupDetailView).Controller(Controller).Kind(ECrowdyGroupKind::Channel) ]

			// Create channel.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("CreateChannelHeader", "Create channel"), TEXT("plus")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[ CrowdyStudioWidgets::Field(LOCTEXT("ChannelNameLabel", "Name"), SAssignNew(NameBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ChannelNameHint", "Channel name"))) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[ CrowdyStudioWidgets::Field(LOCTEXT("ChannelDescLabel", "Description"), SAssignNew(DescriptionBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("ChannelDescHint", "Optional"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SAssignNew(MembersCanSendCheck, SCheckBox).IsChecked(ECheckBoxState::Checked)
							[ SNew(STextBlock).Text(LOCTEXT("MembersCanSend", "Members can send")).TextStyle(&Style, "Crowdy.Text.Body") ]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Primary").ContentPadding(FMargin(13.0f, 7.0f))
							.OnClicked(this, &SCrowdyChannelsView::OnCreateChannelClicked)
							[ SNew(STextBlock).Text(LOCTEXT("CreateChannelButton", "Create Channel")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
						]
					]

					// One-click session channel.
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(11.0f, 9.0f))
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
							[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("SessionChannelExplain", "Pre-create the Reliable-RPC session channel (__crowdy_session_<appId>) with members-can-send and open membership. The runtime also creates it on demand, but seeding it here makes it visible right away.")) ]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(12.0f, 6.0f))
								.OnClicked(this, &SCrowdyChannelsView::OnCreateSessionChannelClicked)
								[ SNew(STextBlock).Text(LOCTEXT("CreateSessionButton", "Create session channel")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
							]
						]
					],
					FMargin(16.0f, 14.0f))
			]

			// Channel policy.
			+ SVerticalBox::Slot().AutoHeight()
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("ChannelPolicyHeader", "Channel policy"), TEXT("config")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(this, &SCrowdyChannelsView::GetPolicyLabel) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(11.0f, 9.0f))
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0.0f, 1.0f, 9.0f, 0.0f)
							[ CrowdyStudioWidgets::Icon(TEXT("broadcast"), 15.0f, FSlateColor(FCrowdyStudioStyle::Info())) ]
							+ SHorizontalBox::Slot().FillWidth(1.0f)
							[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Body").Text(LOCTEXT("SessionChannelHint", "The Reliable-RPC session channel wants: creation = member, default membership = open, and members can send.")) ]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						CrowdyStudioWidgets::Field(LOCTEXT("ChCreation", "Creation policy"),
							CrowdyStudioWidgets::SegmentedEnum(
								{ TEXT("admin"), TEXT("member"), TEXT("anyone") },
								{ LOCTEXT("PolAdmin", "Admin"), LOCTEXT("PolMember", "Member"), LOCTEXT("PolAnyone", "Anyone") },
								TAttribute<FString>::CreateLambda([this]() { return CreationPolicy; }),
								[this](const FString& V) { CreationPolicy = V; }))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						CrowdyStudioWidgets::Field(LOCTEXT("ChMembership", "Default membership"),
							CrowdyStudioWidgets::SegmentedEnum(
								{ TEXT("open"), TEXT("request"), TEXT("invite"), TEXT("admin") },
								{ LOCTEXT("MemOpen", "Open"), LOCTEXT("MemRequest", "Request"), LOCTEXT("MemInvite", "Invite"), LOCTEXT("MemAdmin", "Admin") },
								TAttribute<FString>::CreateLambda([this]() { return MembershipPolicy; }),
								[this](const FString& V) { MembershipPolicy = V; }))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[ CrowdyStudioWidgets::Field(LOCTEXT("ChMaxMembers", "Max members per channel"), SAssignNew(MaxMembersBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("UnlimitedHint", "Blank = unlimited"))) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[ CrowdyStudioWidgets::Field(LOCTEXT("ChMaxGroups", "Max channels per user"), SAssignNew(MaxGroupsBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("UnlimitedHint", "Blank = unlimited"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[
						SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Primary").ContentPadding(FMargin(13.0f, 7.0f))
						.OnClicked(this, &SCrowdyChannelsView::OnSetPolicyClicked)
						[ SNew(STextBlock).Text(LOCTEXT("SetChannelPolicyButton", "Set Policy")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
					],
					FMargin(16.0f, 14.0f))
			]
		]
	];

	if (Controller.IsValid() && Controller->IsSignedIn() && Controller->GetSelectedAppId() != 0)
	{
		RefreshAll();
	}
}

SCrowdyChannelsView::~SCrowdyChannelsView()
{
	if (Controller.IsValid())
	{
		Controller->OnChannelsChanged.RemoveAll(this);
		Controller->OnChannelPolicyChanged.RemoveAll(this);
		Controller->OnSelectedAppChanged.RemoveAll(this);
	}
}

void SCrowdyChannelsView::RefreshAll()
{
	if (Controller.IsValid())
	{
		Controller->FetchChannels();
		Controller->FetchChannelPolicy();
	}
}

void SCrowdyChannelsView::HandleAppChanged()
{
	if (Controller.IsValid())
	{
		Controller->SelectGroup(ECrowdyGroupKind::Channel, 0);
	}
	RefreshAll();
}

FReply SCrowdyChannelsView::OnRefreshClicked()
{
	RefreshAll();
	return FReply::Handled();
}

FReply SCrowdyChannelsView::OnCreateChannelClicked()
{
	if (Controller.IsValid() && NameBox.IsValid())
	{
		Controller->CreateChannel(
			NameBox->GetText().ToString(),
			DescriptionBox.IsValid() ? DescriptionBox->GetText().ToString() : FString(),
			MembersCanSendCheck.IsValid() && MembersCanSendCheck->GetCheckedState() == ECheckBoxState::Checked);
	}
	return FReply::Handled();
}

FReply SCrowdyChannelsView::OnCreateSessionChannelClicked()
{
	if (Controller.IsValid())
	{
		Controller->CreateSessionChannel();
	}
	return FReply::Handled();
}

FReply SCrowdyChannelsView::OnSetPolicyClicked()
{
	if (Controller.IsValid())
	{
		Controller->SetChannelPolicy(CreationPolicy, MembershipPolicy, CrowdyStudioWidgets::ReadCapBox(MaxMembersBox), CrowdyStudioWidgets::ReadCapBox(MaxGroupsBox));
	}
	return FReply::Handled();
}

void SCrowdyChannelsView::OnChannelSelected(TSharedPtr<FStudioGroup> Group, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo == ESelectInfo::Direct)
	{
		return;
	}

	if (Controller.IsValid())
	{
		Controller->SelectGroup(ECrowdyGroupKind::Channel, Group.IsValid() ? Group->GroupId : 0);
	}
}

void SCrowdyChannelsView::HandleChannelsChanged()
{
	if (ChannelListView.IsValid())
	{
		ChannelListView->RequestListRefresh();
	}
}

void SCrowdyChannelsView::HandlePolicyChanged()
{
	if (Controller.IsValid())
	{
		const FStudioGroupPolicy& Policy = Controller->GetChannelPolicy();
		if (!Policy.CreationPolicy.IsEmpty()) { CreationPolicy = Policy.CreationPolicy; }
		if (!Policy.DefaultMembershipPolicy.IsEmpty()) { MembershipPolicy = Policy.DefaultMembershipPolicy; }
		CrowdyStudioWidgets::SetCapBox(MaxMembersBox, Policy.MaxMembers);
		CrowdyStudioWidgets::SetCapBox(MaxGroupsBox, Policy.MaxGroupsPerUser);
	}
}

FText SCrowdyChannelsView::GetPolicyLabel() const
{
	if (Controller.IsValid())
	{
		const FStudioGroupPolicy& Policy = Controller->GetChannelPolicy();
		if (!Policy.CreationPolicy.IsEmpty() || !Policy.DefaultMembershipPolicy.IsEmpty())
		{
			return FText::Format(LOCTEXT("CurChPolicyFmt", "Current: creation = {0},  default membership = {1}"),
				FText::FromString(Policy.CreationPolicy), FText::FromString(Policy.DefaultMembershipPolicy));
		}
	}
	return LOCTEXT("NoChPolicy", "Current policy not loaded. Refresh to read it.");
}

TSharedRef<ITableRow> SCrowdyChannelsView::MakeChannelRow(TSharedPtr<FStudioGroup> Group, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Name = Group.IsValid() ? Group->Name : FString();
	const FString Id = Group.IsValid() ? FString::Printf(TEXT("#%lld"), Group->GroupId) : FString();
	const FString Membership = Group.IsValid() ? Group->MembershipPolicy : FString();
	const FString Status = Group.IsValid() ? Group->Status : FString();

	return SNew(STableRow<TSharedPtr<FStudioGroup>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 8.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).Text(FText::FromString(Name)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ CrowdyStudioWidgets::Chip(FText::FromString(Id)) ]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[ SNew(STextBlock).Text(FText::FromString(Membership)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ CrowdyStudioWidgets::Badge(FText::FromString(Status), CrowdyStudioWidgets::ToneForStatus(Status)) ]
			]
		];
}

#undef LOCTEXT_NAMESPACE
