// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class SEditableTextBox;
class SCheckBox;
class ITableRow;
class STableViewBase;

// Channels pane: list/create channels, one-click the Reliable-RPC session channel, set the per-app
// channel policy, and drill into a channel's members and roles. Channels live on the game plane, so a
// game-capable token (email/password sign-in) is needed for these to authorize.
class SCrowdyChannelsView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyChannelsView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyChannelsView() override;

private:
	FReply OnRefreshClicked();
	FReply OnCreateChannelClicked();
	FReply OnCreateSessionChannelClicked();
	FReply OnSetPolicyClicked();

	void RefreshAll();
	void HandleAppChanged();
	void HandleChannelsChanged();
	void HandlePolicyChanged();
	FText GetPolicyLabel() const;

	void OnChannelSelected(TSharedPtr<FStudioGroup> Group, ESelectInfo::Type SelectInfo);
	TSharedRef<ITableRow> MakeChannelRow(TSharedPtr<FStudioGroup> Group, const TSharedRef<STableViewBase>& OwnerTable);

	TSharedPtr<FCrowdyStudioController> Controller;
	TSharedPtr<SListView<TSharedPtr<FStudioGroup>>> ChannelListView;
	TSharedPtr<SEditableTextBox> NameBox;
	TSharedPtr<SEditableTextBox> DescriptionBox;
	TSharedPtr<SCheckBox> MembersCanSendCheck;

	// Policy. The max boxes hold a number, or are blank for "unlimited".
	TSharedPtr<SEditableTextBox> MaxMembersBox;
	TSharedPtr<SEditableTextBox> MaxGroupsBox;
	FString CreationPolicy = TEXT("member");
	FString MembershipPolicy = TEXT("open");
};
