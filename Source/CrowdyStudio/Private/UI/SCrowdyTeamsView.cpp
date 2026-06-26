// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyTeamsView.h"

#include "Model/FCrowdyStudioController.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "UI/SCrowdyGroupDetailView.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

void SCrowdyTeamsView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;

	if (Controller.IsValid())
	{
		Controller->OnTeamsChanged.AddSP(this, &SCrowdyTeamsView::HandleTeamsChanged);
		Controller->OnTeamPolicyChanged.AddSP(this, &SCrowdyTeamsView::HandlePolicyChanged);
		Controller->OnSelectedAppChanged.AddSP(this, &SCrowdyTeamsView::HandleAppChanged);
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWidget> RefreshButton = SNew(SButton)
		.ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(9.0f, 5.0f))
		.OnClicked(this, &SCrowdyTeamsView::OnRefreshClicked)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ CrowdyStudioWidgets::Icon(TEXT("refresh"), 14.0f, FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[ SNew(STextBlock).Text(LOCTEXT("TeamsRefresh", "Refresh")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::TextSecondary())) ]
		];

	ChildSlot
	[
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[ SNew(STextBlock).Text(LOCTEXT("TeamsHeader", "Teams")).TextStyle(&Style, "Crowdy.Text.Title") ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[ SNew(STextBlock).AutoWrapText(true).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("TeamsPlane", "Teams group players together (guilds, parties, factions). They live on the game server, so sign in with your email and password for these to load (an org token only covers account settings).")) ]

			// Team list.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("TeamsList", "Teams"), TEXT("users"), RefreshButton) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SBox).MinDesiredHeight(120.0f).MaxDesiredHeight(260.0f)
					[
						SNew(SOverlay)
						+ SOverlay::Slot()
						[
							SAssignNew(TeamListView, SListView<TSharedPtr<FStudioGroup>>)
							.ListItemsSource(Controller.IsValid() ? &Controller->GetTeams() : nullptr)
							.OnGenerateRow(this, &SCrowdyTeamsView::MakeTeamRow)
							.OnSelectionChanged(this, &SCrowdyTeamsView::OnTeamSelected)
							.SelectionMode(ESelectionMode::Single)
						]
						+ SOverlay::Slot()
						[
							SNew(SBox)
							.Visibility_Lambda([this]() { return (Controller.IsValid() && Controller->GetTeams().Num() == 0) ? EVisibility::Visible : EVisibility::Collapsed; })
							[ CrowdyStudioWidgets::EmptyState(TEXT("users"), LOCTEXT("NoTeams", "No teams loaded.\nPress Refresh after signing in.")) ]
						]
					],
					FMargin(6.0f), /*bFlat*/ true)
			]

			// Drill-in detail (members + roles of the selected team).
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[ SNew(SCrowdyGroupDetailView).Controller(Controller).Kind(ECrowdyGroupKind::Team) ]

			// Create team.
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("CreateTeamHeader", "Create team"), TEXT("plus")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[ CrowdyStudioWidgets::Field(LOCTEXT("TeamNameLabel", "Name"), SAssignNew(NewTeamNameBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("TeamNameHint", "Team name"))) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[ CrowdyStudioWidgets::Field(LOCTEXT("TeamDescLabel", "Description"), SAssignNew(NewTeamDescBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("TeamDescHint", "Optional"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						CrowdyStudioWidgets::Field(LOCTEXT("TeamCreateMembership", "Default membership"),
							CrowdyStudioWidgets::SegmentedEnum(
								{ TEXT("open"), TEXT("request"), TEXT("invite"), TEXT("admin") },
								{ LOCTEXT("MemOpen", "Open"), LOCTEXT("MemRequest", "Request"), LOCTEXT("MemInvite", "Invite"), LOCTEXT("MemAdmin", "Admin") },
								TAttribute<FString>::CreateLambda([this]() { return NewTeamMembership; }),
								[this](const FString& V) { NewTeamMembership = V; }))
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[
						SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Primary").ContentPadding(FMargin(13.0f, 7.0f))
						.OnClicked(this, &SCrowdyTeamsView::OnCreateTeamClicked)
						[ SNew(STextBlock).Text(LOCTEXT("CreateTeamButton", "Create Team")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
					],
					FMargin(16.0f, 14.0f))
			]

			// Team policy.
			+ SVerticalBox::Slot().AutoHeight()
			[
				CrowdyStudioWidgets::Card(
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("TeamPolicyHeader", "Team policy"), TEXT("config")) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(this, &SCrowdyTeamsView::GetPolicyLabel) ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						CrowdyStudioWidgets::Field(LOCTEXT("TeamCreation", "Creation policy"),
							CrowdyStudioWidgets::SegmentedEnum(
								{ TEXT("admin"), TEXT("member"), TEXT("anyone") },
								{ LOCTEXT("PolAdmin", "Admin"), LOCTEXT("PolMember", "Member"), LOCTEXT("PolAnyone", "Anyone") },
								TAttribute<FString>::CreateLambda([this]() { return CreationPolicy; }),
								[this](const FString& V) { CreationPolicy = V; }))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
					[
						CrowdyStudioWidgets::Field(LOCTEXT("TeamMembership", "Default membership"),
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
						[ CrowdyStudioWidgets::Field(LOCTEXT("TeamMaxMembers", "Max members per team"), SAssignNew(MaxMembersBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("UnlimitedHint", "Blank = unlimited"))) ]
						+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[ CrowdyStudioWidgets::Field(LOCTEXT("TeamMaxGroups", "Max teams per user"), SAssignNew(MaxGroupsBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("UnlimitedHint", "Blank = unlimited"))) ]
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[
						SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Primary").ContentPadding(FMargin(13.0f, 7.0f))
						.OnClicked(this, &SCrowdyTeamsView::OnSetPolicyClicked)
						[ SNew(STextBlock).Text(LOCTEXT("SetTeamPolicyButton", "Set Policy")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
					],
					FMargin(16.0f, 14.0f))
			]
		]
	];

	// Open populated if we are already signed in with an app selected.
	if (Controller.IsValid() && Controller->IsSignedIn() && Controller->GetSelectedAppId() != 0)
	{
		RefreshAll();
	}
}

SCrowdyTeamsView::~SCrowdyTeamsView()
{
	if (Controller.IsValid())
	{
		Controller->OnTeamsChanged.RemoveAll(this);
		Controller->OnTeamPolicyChanged.RemoveAll(this);
		Controller->OnSelectedAppChanged.RemoveAll(this);
	}
}

void SCrowdyTeamsView::RefreshAll()
{
	if (Controller.IsValid())
	{
		Controller->FetchTeams();
		Controller->FetchTeamPolicy();
	}
}

void SCrowdyTeamsView::HandleAppChanged()
{
	// A different app is now active; clear any prior team detail and reload for the new app.
	if (Controller.IsValid())
	{
		Controller->SelectGroup(ECrowdyGroupKind::Team, 0);
	}
	RefreshAll();
}

FReply SCrowdyTeamsView::OnRefreshClicked()
{
	RefreshAll();
	return FReply::Handled();
}

FReply SCrowdyTeamsView::OnSetPolicyClicked()
{
	if (Controller.IsValid())
	{
		Controller->SetTeamPolicy(CreationPolicy, MembershipPolicy, CrowdyStudioWidgets::ReadCapBox(MaxMembersBox), CrowdyStudioWidgets::ReadCapBox(MaxGroupsBox));
	}
	return FReply::Handled();
}

FReply SCrowdyTeamsView::OnCreateTeamClicked()
{
	if (Controller.IsValid() && NewTeamNameBox.IsValid())
	{
		Controller->CreateTeam(
			NewTeamNameBox->GetText().ToString(),
			NewTeamDescBox.IsValid() ? NewTeamDescBox->GetText().ToString() : FString(),
			NewTeamMembership);
	}
	return FReply::Handled();
}

void SCrowdyTeamsView::OnTeamSelected(TSharedPtr<FStudioGroup> Group, ESelectInfo::Type SelectInfo)
{
	// Ignore the selection drop the list emits when its source is rebuilt on refresh; only a real
	// user click should change which team's detail is shown.
	if (SelectInfo == ESelectInfo::Direct)
	{
		return;
	}

	if (Controller.IsValid())
	{
		Controller->SelectGroup(ECrowdyGroupKind::Team, Group.IsValid() ? Group->GroupId : 0);
	}
}

void SCrowdyTeamsView::HandleTeamsChanged()
{
	if (TeamListView.IsValid())
	{
		TeamListView->RequestListRefresh();
	}
}

void SCrowdyTeamsView::HandlePolicyChanged()
{
	if (Controller.IsValid())
	{
		const FStudioGroupPolicy& Policy = Controller->GetTeamPolicy();
		if (!Policy.CreationPolicy.IsEmpty()) { CreationPolicy = Policy.CreationPolicy; }
		if (!Policy.DefaultMembershipPolicy.IsEmpty()) { MembershipPolicy = Policy.DefaultMembershipPolicy; }
		CrowdyStudioWidgets::SetCapBox(MaxMembersBox, Policy.MaxMembers);
		CrowdyStudioWidgets::SetCapBox(MaxGroupsBox, Policy.MaxGroupsPerUser);
	}
}

FText SCrowdyTeamsView::GetPolicyLabel() const
{
	if (Controller.IsValid())
	{
		const FStudioGroupPolicy& Policy = Controller->GetTeamPolicy();
		if (!Policy.CreationPolicy.IsEmpty() || !Policy.DefaultMembershipPolicy.IsEmpty())
		{
			return FText::Format(LOCTEXT("CurPolicyFmt", "Current: creation = {0},  default membership = {1}"),
				FText::FromString(Policy.CreationPolicy), FText::FromString(Policy.DefaultMembershipPolicy));
		}
	}
	return LOCTEXT("NoPolicy", "Current policy not loaded. Refresh to read it.");
}

TSharedRef<ITableRow> SCrowdyTeamsView::MakeTeamRow(TSharedPtr<FStudioGroup> Group, const TSharedRef<STableViewBase>& OwnerTable)
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
