// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Templates/Function.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class SEditableTextBox;

// Project pane: choose an organization, browse its apps, create one, pick the active app, review
// the project's current config against the selected app and sync it, and link the app to a game
// server (the environments list, folded in here). One page for pointing the project at an app.
class SCrowdyProjectView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyProjectView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyProjectView() override;

private:
	FReply OnCreateAppClicked();
	FReply OnSyncClicked();

	void HandleOrganizationsChanged();
	void HandleAppsChanged();

	TSharedRef<SWidget> MakeOrgComboEntry(TSharedPtr<FStudioOrg> Org);
	void OnOrgComboChanged(TSharedPtr<FStudioOrg> Org, ESelectInfo::Type SelectInfo);
	FText GetSelectedOrgLabel() const;

	TSharedRef<ITableRow> MakeAppRow(TSharedPtr<FStudioApp> App, const TSharedRef<STableViewBase>& OwnerTable);
	void OnAppSelected(TSharedPtr<FStudioApp> App, ESelectInfo::Type SelectInfo);

	TSharedPtr<FCrowdyStudioController> Controller;

	TSharedPtr<SComboBox<TSharedPtr<FStudioOrg>>> OrgComboBox;
	TSharedPtr<SListView<TSharedPtr<FStudioApp>>> AppListView;

	TSharedPtr<SEditableTextBox> NameBox;
	TSharedPtr<SEditableTextBox> SlugBox;

	// Selected enum values for the create form (segmented controls write these).
	FString CreateStatus = TEXT("DRAFT");
	FString CreateVisibility = TEXT("PRIVATE");

	// Current UDP protocol choice for the Connection section's segmented control ("Auto"/"IPv4"/
	// "IPv6"), seeded from the settings and mapped back to ECrowdyUDPProtocol on change.
	FString UdpProtocolChoice = TEXT("Auto");
};
