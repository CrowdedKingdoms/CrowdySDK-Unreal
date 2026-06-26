// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;

// Environments view: the harmless, in-editor-useful half. It lists an org's environments, links
// the selected app to one, and syncs the resulting endpoints into the project. Provisioning,
// destroying, and secrets live in the web console (Web Console Environments), so the editor
// never generates or holds the environment's private key.
//
// It is hosted inside the Project page's "Game server" section (Embedded), and can also stand
// alone as its own page if we ever want that again.
class SCrowdyEnvironmentsView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyEnvironmentsView)
		: _Embedded(false)
	{}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		// When true the view is hosted inside the Project page (Game server section): it drops
		// its own page title and the duplicate Sync button, since the Project page owns sync.
		SLATE_ARGUMENT(bool, Embedded)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyEnvironmentsView() override;

private:
	FReply OnRefreshClicked();
	FReply OnLinkClicked();
	FReply OnSyncEndpointsClicked();

	void HandleEnvironmentsChanged();

	TSharedRef<ITableRow> MakeEnvRow(TSharedPtr<FStudioEnvironment> Env, const TSharedRef<STableViewBase>& OwnerTable);
	void OnEnvSelected(TSharedPtr<FStudioEnvironment> Env, ESelectInfo::Type SelectInfo);

	TSharedPtr<FCrowdyStudioController> Controller;
	TSharedPtr<SListView<TSharedPtr<FStudioEnvironment>>> EnvListView;
};
