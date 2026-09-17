// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Templates/Function.h"
#include "UI/CrowdyAppListFilter.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class SCrowdyCreateAppDialog;
class SEditableTextBox;
class SMultiLineEditableTextBox;
class SSearchBox;
class SWidgetSwitcher;

// Project page: an app rail on the left (org filter, search, status filter, the list) and the selected app on
// the right (identity and facts, the actions the management API allows, then the project configuration diff
// and the realtime connection knobs as tabs). Creating an app opens a two-step sheet over the page.
class SCrowdyProjectView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyProjectView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyProjectView() override;

private:
	TSharedRef<SWidget> BuildRail();
	TSharedRef<SWidget> BuildDetail();
	TSharedRef<SWidget> BuildHeaderCard();
	TSharedRef<SWidget> BuildConfigurationTab();
	TSharedRef<SWidget> BuildConnectionTab();

	// A labelled fact for the header grid; the value reads live from the selected app.
	TSharedRef<SWidget> Fact(const FText& Label, TFunction<FString()> Value, bool bCopyable = false, bool bMono = false);
	// A button that drops a short list of choices; picking one runs OnPick with the stored value.
	TSharedRef<SWidget> ChoiceMenu(const FText& Label, const TArray<FString>& Values, const TArray<FText>& Labels,
		TFunction<FString()> Current, TFunction<void(const FString&)> OnPick, TFunction<bool()> Enabled);

	void RefreshVisibleApps();
	void HandleOrganizationsChanged();
	void HandleAppsChanged();

	TSharedRef<SWidget> MakeOrgComboEntry(TSharedPtr<FStudioOrg> Org);
	void OnOrgComboChanged(TSharedPtr<FStudioOrg> Org, ESelectInfo::Type SelectInfo);
	FText GetSelectedOrgLabel() const;

	TSharedRef<ITableRow> MakeAppRow(TSharedPtr<FStudioApp> App, const TSharedRef<STableViewBase>& OwnerTable);
	void OnAppSelected(TSharedPtr<FStudioApp> App, ESelectInfo::Type SelectInfo);

	FReply OnCreateAppClicked();
	FReply OnRefreshClicked();
	FReply OnSyncClicked();
	FReply OnArchiveClicked();
	FReply OnSaveDetailsClicked();

	TSharedPtr<FStudioApp> SelectedApp() const;
	int32 ConfigChangeCount() const;
	// The org a new app would be created in (the filter's, else the selected app's, else the account's only one); 0 when none.
	int64 CreateOrgId() const;

	TSharedPtr<FCrowdyStudioController> Controller;

	// The combo's options: an "every organization" entry (OrgId 0) followed by the account's orgs.
	TArray<TSharedPtr<FStudioOrg>> OrgOptions;
	TSharedPtr<SComboBox<TSharedPtr<FStudioOrg>>> OrgComboBox;

	// Opens on the live apps; the filter is a view choice and is not persisted.
	FCrowdyAppListFilter Filter{ 0, TEXT("LIVE"), FString() };
	TSharedPtr<SSearchBox> SearchBox;
	TArray<TSharedPtr<FStudioApp>> VisibleApps;
	TSharedPtr<SListView<TSharedPtr<FStudioApp>>> AppListView;

	TSharedPtr<SWidgetSwitcher> DetailSwitcher;
	TSharedPtr<SWidgetSwitcher> HeaderSwitcher;
	TSharedPtr<SWidgetSwitcher> TabSwitcher;
	TSharedPtr<SEditableTextBox> EditNameBox;
	TSharedPtr<SMultiLineEditableTextBox> EditDescriptionBox;
	TSharedPtr<SCrowdyCreateAppDialog> CreateDialog;

	FString ActiveTab = TEXT("config");
	// The app whose details are in the edit boxes (0 when not editing). Save refuses if the selection has
	// moved since, and a refresh only drops the edit when it has: the boxes must never save one app's name onto another.
	int64 EditingAppId = 0;

	// The Connection tab's protocol choice ("Auto"/"IPv4"/"IPv6"), seeded from the settings and mapped back to
	// ECrowdyUDPProtocol on change.
	FString UdpProtocolChoice = TEXT("Auto");
};
