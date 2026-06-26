// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class SEditableTextBox;
class ITableRow;
class STableViewBase;

// Teams pane: list the selected app's teams, create teams, set the per-app team policy, and drill into
// a team's members and roles. Teams live on the game plane, so a game-capable token (email/password
// sign-in) is needed for these to authorize.
class SCrowdyTeamsView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyTeamsView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyTeamsView() override;

private:
	FReply OnRefreshClicked();
	FReply OnSetPolicyClicked();
	FReply OnCreateTeamClicked();

	void RefreshAll();
	void HandleAppChanged();
	void HandleTeamsChanged();
	void HandlePolicyChanged();
	FText GetPolicyLabel() const;

	void OnTeamSelected(TSharedPtr<FStudioGroup> Group, ESelectInfo::Type SelectInfo);
	TSharedRef<ITableRow> MakeTeamRow(TSharedPtr<FStudioGroup> Group, const TSharedRef<STableViewBase>& OwnerTable);

	TSharedPtr<FCrowdyStudioController> Controller;
	TSharedPtr<SListView<TSharedPtr<FStudioGroup>>> TeamListView;

	// Create team.
	TSharedPtr<SEditableTextBox> NewTeamNameBox;
	TSharedPtr<SEditableTextBox> NewTeamDescBox;
	FString NewTeamMembership = TEXT("open");

	// Policy. The max boxes hold a number, or are blank for "unlimited".
	TSharedPtr<SEditableTextBox> MaxMembersBox;
	TSharedPtr<SEditableTextBox> MaxGroupsBox;
	FString CreationPolicy = TEXT("member");
	FString MembershipPolicy = TEXT("open");
};
