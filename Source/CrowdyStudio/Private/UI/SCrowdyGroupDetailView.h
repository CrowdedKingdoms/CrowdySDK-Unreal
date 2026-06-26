// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class SBox;
class SEditableTextBox;
class ITableRow;
class STableViewBase;

// Shared read + edit detail panel for a selected team or channel: its members and roles, with
// add/remove member, set member roles, and create/delete/edit role controls. The Teams and Channels
// views embed this with their own Kind; every server call routes through the controller (game plane).
// Read-only-ness is the server's call here: the controls always show, and a permission the signed-in
// token lacks surfaces as a rejection toast (same model as the rest of the game-plane authoring).
class SCrowdyGroupDetailView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyGroupDetailView)
		: _Kind(ECrowdyGroupKind::Team)
	{}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		SLATE_ARGUMENT(ECrowdyGroupKind, Kind)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyGroupDetailView() override;

private:
	// Detail (members + roles) reloaded on the controller.
	void HandleDetailChanged();
	// Re-point the selection panels at the freshly fetched data (or clear them if the selected
	// member/role no longer exists), so the checkboxes always reflect server truth after an edit.
	void ResyncPanels();

	TSharedRef<ITableRow> MakeMemberRow(TSharedPtr<FStudioGroupMember> Member, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakeRoleRow(TSharedPtr<FStudioGroupRole> Role, const TSharedRef<STableViewBase>& OwnerTable);

	void OnMemberSelected(TSharedPtr<FStudioGroupMember> Member, ESelectInfo::Type SelectInfo);
	void OnRoleSelected(TSharedPtr<FStudioGroupRole> Role, ESelectInfo::Type SelectInfo);

	// A wrap box of checkboxes over the FIXED group-permission set, each bound live to *Backing.
	TSharedRef<SWidget> BuildPermChecklist(TSet<FString>* Backing);
	void RebuildMemberRolesPanel();
	void RebuildRoleEditPanel();

	FReply OnAddMemberClicked();
	FReply OnApplyMemberRolesClicked();
	FReply OnCreateRoleClicked();
	FReply OnSaveRoleClicked();
	FReply OnRemoveMemberClicked(int64 InUserId);
	FReply OnApproveMemberClicked(int64 InUserId);
	FReply OnDeleteRoleClicked(int64 GroupRoleId, FString RoleName);
	FReply OnDeleteGroupClicked();
	FReply OnSaveGroupClicked();

	bool HasGroup() const;
	int64 GroupId() const;

	TSharedPtr<FCrowdyStudioController> Controller;
	ECrowdyGroupKind Kind = ECrowdyGroupKind::Team;

	TSharedPtr<SListView<TSharedPtr<FStudioGroupMember>>> MemberListView;
	TSharedPtr<SListView<TSharedPtr<FStudioGroupRole>>> RoleListView;

	TSharedPtr<SEditableTextBox> AddUserIdBox;
	TSharedPtr<SEditableTextBox> NewRoleNameBox;
	TSharedPtr<SEditableTextBox> NewRoleRankBox;

	// Edit-group (rename / description / membership) form for the selected team/channel.
	TSharedPtr<SEditableTextBox> EditNameBox;
	TSharedPtr<SEditableTextBox> EditDescBox;
	FString EditMembership;   // "" = keep current

	// Edit-role name/rank inputs, rebuilt with the selected role's current values on selection.
	TSharedPtr<SEditableTextBox> RoleEditNameBox;
	TSharedPtr<SEditableTextBox> RoleEditRankBox;
	FString RoleEditName;
	int32 RoleEditRank = 0;

	TSharedPtr<SBox> MemberRolesHost;   // role checkboxes for the currently selected member
	TSharedPtr<SBox> RoleEditHost;      // permission checkboxes for the currently selected role

	int64 SelectedMemberUserId = 0;
	int64 SelectedRoleId = 0;
	bool bSelectedRoleIsSystem = false;

	TSet<int64> MemberRoleSelection;    // role ids to assign to the selected member
	TSet<FString> RoleEditPerms;        // permissions of the selected existing role
	TSet<FString> NewRolePerms;         // permissions for the create-role form
};
