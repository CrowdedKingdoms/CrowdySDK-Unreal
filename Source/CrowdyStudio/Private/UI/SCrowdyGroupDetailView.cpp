// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/SCrowdyGroupDetailView.h"

#include "Model/FCrowdyStudioController.h"
#include "Misc/MessageDialog.h"
#include "Style/CrowdyStudioStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/StyleDefaults.h"
#include "UI/CrowdyStudioWidgets.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CrowdyStudio"

namespace
{
	// The FIXED group-management permission set (NOT the runtime permission catalog). A role grants
	// some subset of these; the labels are the plain-language captions shown beside each checkbox.
	struct FGroupPermInfo { const TCHAR* Key; const TCHAR* Label; };
	const TArray<FGroupPermInfo>& GroupPermCatalog()
	{
		static const TArray<FGroupPermInfo> Perms = {
			{ TEXT("manage_members"), TEXT("Manage members") },
			{ TEXT("manage_roles"),   TEXT("Manage roles") },
			{ TEXT("manage_group"),   TEXT("Manage group") },
			{ TEXT("send_messages"),  TEXT("Send messages") },
		};
		return Perms;
	}
}

void SCrowdyGroupDetailView::Construct(const FArguments& InArgs)
{
	Controller = InArgs._Controller;
	Kind = InArgs._Kind;

	if (Controller.IsValid())
	{
		Controller->OnGroupDetailChanged.AddSP(this, &SCrowdyGroupDetailView::HandleDetailChanged);
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();
	const bool bChannelKind = (Kind == ECrowdyGroupKind::Channel);

	// Edit card: rename / re-describe / change membership of the team/channel itself. Blank fields are
	// left unchanged (partial update), so this doubles as a quick "rename" without disturbing the rest.
	TSharedRef<SWidget> EditCard = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[ CrowdyStudioWidgets::SectionHeader(bChannelKind ? LOCTEXT("EditChannelHeader", "Edit channel") : LOCTEXT("EditTeamHeader", "Edit team"), TEXT("config")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[ CrowdyStudioWidgets::Field(LOCTEXT("EditNameLabel", "Name"), SAssignNew(EditNameBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("EditKeepHint", "Blank = keep"))) ]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[ CrowdyStudioWidgets::Field(LOCTEXT("EditDescLabel", "Description"), SAssignNew(EditDescBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("EditKeepHint", "Blank = keep"))) ]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)
		[
			CrowdyStudioWidgets::Field(LOCTEXT("EditMembershipLabel", "Membership"),
				CrowdyStudioWidgets::SegmentedEnum(
					{ TEXT(""), TEXT("open"), TEXT("request"), TEXT("invite"), TEXT("admin") },
					{ LOCTEXT("MemKeep", "Keep"), LOCTEXT("MemOpen", "Open"), LOCTEXT("MemRequest", "Request"), LOCTEXT("MemInvite", "Invite"), LOCTEXT("MemAdmin", "Admin") },
					TAttribute<FString>::CreateLambda([this]() { return EditMembership; }),
					[this](const FString& V) { EditMembership = V; }))
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(13.0f, 7.0f))
			.OnClicked(this, &SCrowdyGroupDetailView::OnSaveGroupClicked)
			[ SNew(STextBlock).Text(LOCTEXT("SaveGroupBtn", "Save Changes")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
		],
		FMargin(16.0f, 14.0f));

	// Members card.
	TSharedRef<SWidget> MembersCard = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("MembersHeader", "Members"), TEXT("users")) ]

		// Add member.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[ CrowdyStudioWidgets::Field(LOCTEXT("AddMemberLabel", "Add member by user id"), SAssignNew(AddUserIdBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("AddMemberHint", "User id"))) ]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(13.0f, 7.0f))
				.OnClicked(this, &SCrowdyGroupDetailView::OnAddMemberClicked)
				[ SNew(STextBlock).Text(LOCTEXT("AddMemberBtn", "Add")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			]
		]

		// Member list.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBox).MinDesiredHeight(96.0f).MaxDesiredHeight(220.0f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SAssignNew(MemberListView, SListView<TSharedPtr<FStudioGroupMember>>)
					.ListItemsSource(Controller.IsValid() ? &Controller->GetGroupMembers() : nullptr)
					.OnGenerateRow(this, &SCrowdyGroupDetailView::MakeMemberRow)
					.OnSelectionChanged(this, &SCrowdyGroupDetailView::OnMemberSelected)
					.SelectionMode(ESelectionMode::Single)
				]
				+ SOverlay::Slot()
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return (Controller.IsValid() && Controller->GetGroupMembers().Num() == 0) ? EVisibility::Visible : EVisibility::Collapsed; })
					[ CrowdyStudioWidgets::EmptyState(TEXT("users"), LOCTEXT("NoMembers", "No members.")) ]
				]
			]
		]

		// Roles-for-selected-member panel (rebuilt on selection).
		+ SVerticalBox::Slot().AutoHeight()
		[ SAssignNew(MemberRolesHost, SBox) ],
		FMargin(16.0f, 14.0f));

	// Roles card.
	TSharedRef<SWidget> RolesCard = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[ CrowdyStudioWidgets::SectionHeader(LOCTEXT("RolesHeader", "Roles"), TEXT("config")) ]

		// Create role.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[ CrowdyStudioWidgets::Field(LOCTEXT("NewRoleLabel", "New role name"), SAssignNew(NewRoleNameBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("NewRoleHint", "e.g. Moderator"))) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[ CrowdyStudioWidgets::Field(LOCTEXT("NewRoleRankLabel", "Rank (higher = more senior)"), SAssignNew(NewRoleRankBox, SEditableTextBox).Style(&Style, "Crowdy.Input").HintText(LOCTEXT("NewRoleRankHint", "0"))) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("NewRolePermsLabel", "Permissions this role grants:")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[ BuildPermChecklist(&NewRolePerms) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f).HAlign(HAlign_Right)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(13.0f, 7.0f))
			.OnClicked(this, &SCrowdyGroupDetailView::OnCreateRoleClicked)
			[ SNew(STextBlock).Text(LOCTEXT("CreateRoleBtn", "Create Role")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor::UseForeground()) ]
		]

		// Role list.
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBox).MinDesiredHeight(96.0f).MaxDesiredHeight(220.0f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SAssignNew(RoleListView, SListView<TSharedPtr<FStudioGroupRole>>)
					.ListItemsSource(Controller.IsValid() ? &Controller->GetGroupRoles() : nullptr)
					.OnGenerateRow(this, &SCrowdyGroupDetailView::MakeRoleRow)
					.OnSelectionChanged(this, &SCrowdyGroupDetailView::OnRoleSelected)
					.SelectionMode(ESelectionMode::Single)
				]
				+ SOverlay::Slot()
				[
					SNew(SBox)
					.Visibility_Lambda([this]() { return (Controller.IsValid() && Controller->GetGroupRoles().Num() == 0) ? EVisibility::Visible : EVisibility::Collapsed; })
					[ CrowdyStudioWidgets::EmptyState(TEXT("config"), LOCTEXT("NoRoles", "No roles.")) ]
				]
			]
		]

		// Edit-selected-role panel (rebuilt on selection).
		+ SVerticalBox::Slot().AutoHeight()
		[ SAssignNew(RoleEditHost, SBox) ],
		FMargin(16.0f, 14.0f));

	// Danger zone: disband the whole team/channel. Mirrors the member/role delete confirm flow.
	TSharedRef<SWidget> DangerCard = CrowdyStudioWidgets::Card(
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.BodyStrong").Text(LOCTEXT("DangerHeader", "Danger zone")) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
		[
			SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").AutoWrapText(true)
			.Text(bChannelKind
				? LOCTEXT("DeleteChannelHint", "Deleting a channel removes all members and roles and tears down its message routing. This cannot be undone.")
				: LOCTEXT("DeleteTeamHint", "Deleting a team removes all members and roles and revokes any grid grants it conferred. This cannot be undone."))
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(13.0f, 7.0f))
			.OnClicked(this, &SCrowdyGroupDetailView::OnDeleteGroupClicked)
			[
				SNew(STextBlock)
				.Text(bChannelKind ? LOCTEXT("DeleteChannelBtn", "Delete Channel") : LOCTEXT("DeleteTeamBtn", "Delete Team"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger()))
			]
		],
		FMargin(16.0f, 14.0f));

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox)
			.Visibility_Lambda([this]() { return HasGroup() ? EVisibility::Collapsed : EVisibility::Visible; })
			[ CrowdyStudioWidgets::EmptyState(TEXT("users"), LOCTEXT("NoGroupSelected", "Select a row above to view and edit its members and roles.")) ]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SBox)
			.Visibility_Lambda([this]() { return HasGroup() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)[ EditCard ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 12.0f)[ MembersCard ]
				+ SVerticalBox::Slot().AutoHeight()[ RolesCard ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 12.0f, 0.0f, 0.0f)[ DangerCard ]
			]
		]
	];
}

SCrowdyGroupDetailView::~SCrowdyGroupDetailView()
{
	if (Controller.IsValid())
	{
		Controller->OnGroupDetailChanged.RemoveAll(this);
	}
}

bool SCrowdyGroupDetailView::HasGroup() const
{
	return Controller.IsValid() && Controller->GetSelectedGroupId() != 0 && Controller->GetSelectedGroupKind() == Kind;
}

int64 SCrowdyGroupDetailView::GroupId() const
{
	return Controller.IsValid() ? Controller->GetSelectedGroupId() : 0;
}

void SCrowdyGroupDetailView::HandleDetailChanged()
{
	if (MemberListView.IsValid())
	{
		MemberListView->RequestListRefresh();
	}
	if (RoleListView.IsValid())
	{
		RoleListView->RequestListRefresh();
	}
	ResyncPanels();
}

void SCrowdyGroupDetailView::ResyncPanels()
{
	if (!Controller.IsValid())
	{
		return;
	}

	// Member roles panel: keep it pointed at the selected member if they still exist, re-deriving the
	// checked set from the server data; otherwise clear it.
	bool bMemberStillPresent = false;
	if (SelectedMemberUserId != 0)
	{
		for (const TSharedPtr<FStudioGroupMember>& Member : Controller->GetGroupMembers())
		{
			if (Member.IsValid() && Member->UserId == SelectedMemberUserId)
			{
				MemberRoleSelection.Reset();
				MemberRoleSelection.Append(Member->RoleIds);
				bMemberStillPresent = true;
				break;
			}
		}
	}
	if (!bMemberStillPresent)
	{
		SelectedMemberUserId = 0;
		MemberRoleSelection.Reset();
	}
	RebuildMemberRolesPanel();

	// Role edit panel: same idea for the selected role.
	bool bRoleStillPresent = false;
	if (SelectedRoleId != 0)
	{
		for (const TSharedPtr<FStudioGroupRole>& Role : Controller->GetGroupRoles())
		{
			if (Role.IsValid() && Role->GroupRoleId == SelectedRoleId)
			{
				RoleEditPerms.Reset();
				RoleEditPerms.Append(Role->Permissions);
				bSelectedRoleIsSystem = Role->bIsSystem;
				RoleEditName = Role->RoleName;
				RoleEditRank = Role->Rank;
				bRoleStillPresent = true;
				break;
			}
		}
	}
	if (!bRoleStillPresent)
	{
		SelectedRoleId = 0;
		RoleEditPerms.Reset();
	}
	RebuildRoleEditPanel();
}

TSharedRef<SWidget> SCrowdyGroupDetailView::BuildPermChecklist(TSet<FString>* Backing)
{
	TSharedRef<SWrapBox> Wrap = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(10.0f, 6.0f));

	for (const FGroupPermInfo& Perm : GroupPermCatalog())
	{
		const FString Key = Perm.Key;
		Wrap->AddSlot()
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([Backing, Key]() { return Backing->Contains(Key) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Backing, Key](ECheckBoxState NewState)
			{
				if (NewState == ECheckBoxState::Checked) { Backing->Add(Key); }
				else { Backing->Remove(Key); }
			})
			[ SNew(STextBlock).Text(FText::FromString(Perm.Label)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Body") ]
		];
	}

	return Wrap;
}

void SCrowdyGroupDetailView::RebuildMemberRolesPanel()
{
	if (!MemberRolesHost.IsValid())
	{
		return;
	}

	if (SelectedMemberUserId == 0)
	{
		MemberRolesHost->SetContent(SNullWidget::NullWidget);
		return;
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	TSharedRef<SWrapBox> RolesWrap = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(10.0f, 6.0f));
	if (Controller.IsValid())
	{
		for (const TSharedPtr<FStudioGroupRole>& Role : Controller->GetGroupRoles())
		{
			if (!Role.IsValid())
			{
				continue;
			}
			const int64 RoleId = Role->GroupRoleId;
			RolesWrap->AddSlot()
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this, RoleId]() { return MemberRoleSelection.Contains(RoleId) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this, RoleId](ECheckBoxState NewState)
				{
					if (NewState == ECheckBoxState::Checked) { MemberRoleSelection.Add(RoleId); }
					else { MemberRoleSelection.Remove(RoleId); }
				})
				[ SNew(STextBlock).Text(FText::FromString(Role->RoleName)).TextStyle(&Style, "Crowdy.Text.Body") ]
			];
		}
	}

	const bool bHasRoles = Controller.IsValid() && Controller->GetGroupRoles().Num() > 0;

	MemberRolesHost->SetContent(
		SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(11.0f, 9.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.BodyStrong").Text(FText::Format(LOCTEXT("RolesForUserFmt", "Roles for user #{0}"), FText::FromString(FString::Printf(TEXT("%lld"), SelectedMemberUserId)))) ]
			+ SVerticalBox::Slot().AutoHeight()
			[
				bHasRoles
					? StaticCastSharedRef<SWidget>(RolesWrap)
					: StaticCastSharedRef<SWidget>(SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("NoRolesToAssign", "No roles defined yet. Create one below to assign it.")))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f).HAlign(HAlign_Right)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(12.0f, 6.0f))
				.IsEnabled(bHasRoles)
				.OnClicked(this, &SCrowdyGroupDetailView::OnApplyMemberRolesClicked)
				[ SNew(STextBlock).Text(LOCTEXT("ApplyRolesBtn", "Apply roles")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			]
		]);
}

void SCrowdyGroupDetailView::RebuildRoleEditPanel()
{
	if (!RoleEditHost.IsValid())
	{
		return;
	}

	if (SelectedRoleId == 0)
	{
		RoleEditHost->SetContent(SNullWidget::NullWidget);
		return;
	}

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	if (bSelectedRoleIsSystem)
	{
		RoleEditHost->SetContent(
			SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(11.0f, 9.0f))
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("SystemRoleReadOnly", "System roles cannot be edited or deleted.")) ]);
		return;
	}

	RoleEditHost->SetContent(
		SNew(SBorder).BorderImage(Style.GetBrush("Crowdy.Inset")).Padding(FMargin(11.0f, 9.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.BodyStrong").Text(LOCTEXT("EditRoleHeader", "Edit the selected role")) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(2.0f).Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[ CrowdyStudioWidgets::Field(LOCTEXT("EditRoleNameLabel", "Name"), SAssignNew(RoleEditNameBox, SEditableTextBox).Style(&Style, "Crowdy.Input").Text(FText::FromString(RoleEditName))) ]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[ CrowdyStudioWidgets::Field(LOCTEXT("EditRoleRankLabel", "Rank"), SAssignNew(RoleEditRankBox, SEditableTextBox).Style(&Style, "Crowdy.Input").Text(FText::FromString(FString::FromInt(RoleEditRank)))) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[ SNew(STextBlock).TextStyle(&Style, "Crowdy.Text.Subtle").Text(LOCTEXT("EditRolePermsLabel", "Permissions this role grants:")) ]
			+ SVerticalBox::Slot().AutoHeight()
			[ BuildPermChecklist(&RoleEditPerms) ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f).HAlign(HAlign_Right)
			[
				SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(12.0f, 6.0f))
				.OnClicked(this, &SCrowdyGroupDetailView::OnSaveRoleClicked)
				[ SNew(STextBlock).Text(LOCTEXT("SaveRoleBtn", "Save Role")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
			]
		]);
}

void SCrowdyGroupDetailView::OnMemberSelected(TSharedPtr<FStudioGroupMember> Member, ESelectInfo::Type SelectInfo)
{
	// Ignore programmatic clears (the list drops its selection when its source array is rebuilt after
	// an edit); only a real user click should change which member is in the panel.
	if (SelectInfo == ESelectInfo::Direct)
	{
		return;
	}

	if (Member.IsValid())
	{
		SelectedMemberUserId = Member->UserId;
		MemberRoleSelection.Reset();
		MemberRoleSelection.Append(Member->RoleIds);
	}
	else
	{
		SelectedMemberUserId = 0;
		MemberRoleSelection.Reset();
	}
	RebuildMemberRolesPanel();
}

void SCrowdyGroupDetailView::OnRoleSelected(TSharedPtr<FStudioGroupRole> Role, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo == ESelectInfo::Direct)
	{
		return;
	}

	if (Role.IsValid())
	{
		SelectedRoleId = Role->GroupRoleId;
		bSelectedRoleIsSystem = Role->bIsSystem;
		RoleEditPerms.Reset();
		RoleEditPerms.Append(Role->Permissions);
		RoleEditName = Role->RoleName;
		RoleEditRank = Role->Rank;
	}
	else
	{
		SelectedRoleId = 0;
		bSelectedRoleIsSystem = false;
		RoleEditPerms.Reset();
		RoleEditName.Reset();
		RoleEditRank = 0;
	}
	RebuildRoleEditPanel();
}

FReply SCrowdyGroupDetailView::OnAddMemberClicked()
{
	if (Controller.IsValid() && AddUserIdBox.IsValid())
	{
		const int64 InUserId = FCString::Atoi64(*AddUserIdBox->GetText().ToString());
		Controller->AddGroupMember(Kind, GroupId(), InUserId);
		AddUserIdBox->SetText(FText::GetEmpty());
	}
	return FReply::Handled();
}

FReply SCrowdyGroupDetailView::OnApplyMemberRolesClicked()
{
	if (Controller.IsValid() && SelectedMemberUserId != 0)
	{
		Controller->SetGroupMemberRoles(Kind, GroupId(), SelectedMemberUserId, MemberRoleSelection.Array());
	}
	return FReply::Handled();
}

FReply SCrowdyGroupDetailView::OnCreateRoleClicked()
{
	if (Controller.IsValid() && NewRoleNameBox.IsValid())
	{
		const int32 Rank = NewRoleRankBox.IsValid() ? FCString::Atoi(*NewRoleRankBox->GetText().ToString()) : 0;
		Controller->CreateGroupRole(Kind, GroupId(), NewRoleNameBox->GetText().ToString(), NewRolePerms.Array(), Rank);
		NewRoleNameBox->SetText(FText::GetEmpty());
		if (NewRoleRankBox.IsValid()) { NewRoleRankBox->SetText(FText::GetEmpty()); }
		NewRolePerms.Reset();
	}
	return FReply::Handled();
}

FReply SCrowdyGroupDetailView::OnSaveRoleClicked()
{
	if (Controller.IsValid() && SelectedRoleId != 0 && !bSelectedRoleIsSystem)
	{
		const FString NewName = RoleEditNameBox.IsValid() ? RoleEditNameBox->GetText().ToString() : FString();
		const int32 NewRank = RoleEditRankBox.IsValid() ? FCString::Atoi(*RoleEditRankBox->GetText().ToString()) : RoleEditRank;
		Controller->UpdateGroupRole(Kind, SelectedRoleId, NewName, RoleEditPerms.Array(), NewRank);
	}
	return FReply::Handled();
}

FReply SCrowdyGroupDetailView::OnRemoveMemberClicked(int64 InUserId)
{
	if (!Controller.IsValid() || InUserId == 0)
	{
		return FReply::Handled();
	}

	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo,
		FText::Format(LOCTEXT("ConfirmRemoveMember", "Remove user #{0} from this group?"), FText::AsNumber(InUserId)));
	if (Choice == EAppReturnType::Yes)
	{
		Controller->RemoveGroupMember(Kind, GroupId(), InUserId);
	}
	return FReply::Handled();
}

FReply SCrowdyGroupDetailView::OnApproveMemberClicked(int64 InUserId)
{
	if (Controller.IsValid() && InUserId != 0)
	{
		// Approving a pending request is just adding the member: addTeamMember/addChannelMember
		// upserts a pending membership to active.
		Controller->AddGroupMember(Kind, GroupId(), InUserId);
	}
	return FReply::Handled();
}

FReply SCrowdyGroupDetailView::OnDeleteRoleClicked(int64 GroupRoleId, FString RoleName)
{
	if (!Controller.IsValid() || GroupRoleId == 0)
	{
		return FReply::Handled();
	}

	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo,
		FText::Format(LOCTEXT("ConfirmDeleteRole", "Delete role \"{0}\"? This removes it from every member."), FText::FromString(RoleName)));
	if (Choice == EAppReturnType::Yes)
	{
		Controller->DeleteGroupRole(Kind, GroupRoleId);
	}
	return FReply::Handled();
}

FReply SCrowdyGroupDetailView::OnDeleteGroupClicked()
{
	if (!Controller.IsValid() || GroupId() == 0)
	{
		return FReply::Handled();
	}

	const bool bChannel = (Kind == ECrowdyGroupKind::Channel);
	const EAppReturnType::Type Choice = FMessageDialog::Open(EAppMsgType::YesNo,
		bChannel
			? LOCTEXT("ConfirmDeleteChannel", "Delete this channel? This removes all members and roles and tears down its message routing. This cannot be undone.")
			: LOCTEXT("ConfirmDeleteTeam", "Delete this team? This removes all members and roles and revokes any grid grants it conferred. This cannot be undone."));
	if (Choice == EAppReturnType::Yes)
	{
		Controller->DeleteGroup(Kind, GroupId());
	}
	return FReply::Handled();
}

FReply SCrowdyGroupDetailView::OnSaveGroupClicked()
{
	if (!Controller.IsValid() || GroupId() == 0)
	{
		return FReply::Handled();
	}

	const FString NewName = EditNameBox.IsValid() ? EditNameBox->GetText().ToString() : FString();
	const FString NewDesc = EditDescBox.IsValid() ? EditDescBox->GetText().ToString() : FString();
	Controller->UpdateGroup(Kind, GroupId(), NewName, NewDesc, EditMembership);

	// Reset the form to its "keep everything" default after submitting.
	if (EditNameBox.IsValid()) { EditNameBox->SetText(FText::GetEmpty()); }
	if (EditDescBox.IsValid()) { EditDescBox->SetText(FText::GetEmpty()); }
	EditMembership.Reset();
	return FReply::Handled();
}

TSharedRef<ITableRow> SCrowdyGroupDetailView::MakeMemberRow(TSharedPtr<FStudioGroupMember> Member, const TSharedRef<STableViewBase>& OwnerTable)
{
	const int64 InUserId = Member.IsValid() ? Member->UserId : 0;
	const FString Status = Member.IsValid() ? Member->Status : FString();
	const FString Roles = (Member.IsValid() && Member->RoleNames.Num() > 0) ? FString::Join(Member->RoleNames, TEXT(", ")) : FString(TEXT("no roles"));
	const bool bPending = (Status == TEXT("pending"));

	const ISlateStyle& Style = FCrowdyStudioStyle::Get();

	// Pending join requests show inline in the member list (they come back from teamMembers with a
	// "pending" status). Give those rows an Approve button (add the member, which upserts to active),
	// and label their Remove as "Reject"; active members just get Remove.
	TSharedRef<SHorizontalBox> Actions = SNew(SHorizontalBox);
	if (bPending)
	{
		Actions->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 6.0f, 0.0f)
		[
			SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(9.0f, 4.0f))
			.OnClicked(this, &SCrowdyGroupDetailView::OnApproveMemberClicked, InUserId)
			[ SNew(STextBlock).Text(LOCTEXT("ApproveMemberBtn", "Approve")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 9)).ColorAndOpacity(FSlateColor::UseForeground()) ]
		];
	}
	Actions->AddSlot().AutoWidth().VAlign(VAlign_Center)
	[
		SNew(SButton).ButtonStyle(&Style, "Crowdy.Button.Secondary").ContentPadding(FMargin(9.0f, 4.0f))
		.OnClicked(this, &SCrowdyGroupDetailView::OnRemoveMemberClicked, InUserId)
		[ SNew(STextBlock).Text(bPending ? LOCTEXT("RejectMemberBtn", "Reject") : LOCTEXT("RemoveMemberBtn", "Remove")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger())) ]
	];

	return SNew(STableRow<TSharedPtr<FStudioGroupMember>>, OwnerTable)
		.Style(&Style, "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[ SNew(STextBlock).Text(FText::Format(LOCTEXT("MemberUserFmt", "User #{0}"), FText::FromString(FString::Printf(TEXT("%lld"), InUserId)))).TextStyle(&Style, "Crowdy.Text.BodyStrong") ]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[ SNew(STextBlock).Text(FText::FromString(Roles)).TextStyle(&Style, "Crowdy.Text.Subtle") ]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 8.0f, 0.0f)
				[ CrowdyStudioWidgets::Badge(FText::FromString(Status), CrowdyStudioWidgets::ToneForStatus(Status)) ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ Actions ]
			]
		];
}

TSharedRef<ITableRow> SCrowdyGroupDetailView::MakeRoleRow(TSharedPtr<FStudioGroupRole> Role, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString RoleName = Role.IsValid() ? Role->RoleName : FString();
	const bool bIsSystem = Role.IsValid() && Role->bIsSystem;
	const int64 GroupRoleId = Role.IsValid() ? Role->GroupRoleId : 0;
	const FString Perms = (Role.IsValid() && Role->Permissions.Num() > 0) ? FString::Join(Role->Permissions, TEXT(", ")) : FString(TEXT("no permissions"));

	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[ SNew(STextBlock).Text(FText::FromString(RoleName)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.BodyStrong") ]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[ CrowdyStudioWidgets::Badge(bIsSystem ? LOCTEXT("RoleSystem", "system") : LOCTEXT("RoleCustom", "custom"), bIsSystem ? CrowdyStudioWidgets::EBadgeTone::Info : CrowdyStudioWidgets::EBadgeTone::Neutral) ]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[ SNew(STextBlock).Text(FText::FromString(Perms)).TextStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Text.Subtle") ]
		];

	// Only custom roles can be deleted; system roles (e.g. leader) are protected server-side.
	if (!bIsSystem)
	{
		Row->AddSlot().AutoWidth().VAlign(VAlign_Center)
		[
			SNew(SButton).ButtonStyle(&FCrowdyStudioStyle::Get(), "Crowdy.Button.Secondary").ContentPadding(FMargin(9.0f, 4.0f))
			.OnClicked(this, &SCrowdyGroupDetailView::OnDeleteRoleClicked, GroupRoleId, RoleName)
			[ SNew(STextBlock).Text(LOCTEXT("DeleteRoleBtn", "Delete")).Font(FCoreStyle::GetDefaultFontStyle("Regular", 9)).ColorAndOpacity(FSlateColor(FCrowdyStudioStyle::Danger())) ]
		];
	}

	return SNew(STableRow<TSharedPtr<FStudioGroupRole>>, OwnerTable)
		.Style(&FCrowdyStudioStyle::Get(), "Crowdy.TableRow")
		.Padding(FMargin(0.0f, 1.0f))
		[
			SNew(SBorder).BorderImage(FStyleDefaults::GetNoBrush()).Padding(FMargin(11.0f, 7.0f))
			[ Row ]
		];
}

#undef LOCTEXT_NAMESPACE
