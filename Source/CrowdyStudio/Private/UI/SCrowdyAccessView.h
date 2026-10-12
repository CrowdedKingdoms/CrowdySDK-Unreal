// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FActiveTimerHandle;
class FCrowdyStudioController;
class SEditableTextBox;
class SSearchBox;
class SWidgetSwitcher;

// Access page: the selected app's feature keys in a rail, and for the selected feature, which access tiers grant it.
// The three lists load themselves the first time the page is shown for an app.
class SCrowdyAccessView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyAccessView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyAccessView() override;

private:
	TSharedRef<SWidget> BuildRail();
	TSharedRef<SWidget> BuildDetail();
	TSharedRef<SWidget> BuildDefineForm();
	TSharedRef<SWidget> BuildFeatureDetail();

	FReply OnRefreshClicked();
	FReply OnDefineClicked();
	FReply OnSaveFeatureClicked();
	FReply OnCancelDefineClicked();

	// Ask the controller for this app's lists, once, the next time this page is painted. Every Studio page is built up
	// front into one switcher, so this page exists from startup whether or not anyone opens it. An active timer only
	// runs while its widget is being painted, and a widget in an inactive switcher slot never is, so registering one
	// is exactly "when this page is actually on screen".
	void ScheduleEnsureLists();
	EActiveTimerReturnType HandleEnsureLists(double InCurrentTime, float InDeltaTime);
	TWeakPtr<FActiveTimerHandle> EnsureListsTimerHandle;

	// The active app changed and its token was minted. Arms the load again rather than issuing it here: the
	// announcement arrives whether or not this page is the one on screen.
	void HandleAppChanged();
	// Empty the editors and the selection if they were made for an app other than the selected one. Called from the
	// app-changed announcement and from each list announcement, because a mint can fail and leave them describing the
	// previous app with no announcement made. Comparing the app id makes running it twice a no-op.
	void SyncEditorAppScope();
	void ClearAppScopedEditors();

	void HandleFeaturesChanged();
	void HandleAccessListsChanged();
	void RefreshVisibleFeatures();
	void ShowDetail();

	void OnFeatureSelected(TSharedPtr<FStudioAppFeature> Feature, ESelectInfo::Type SelectInfo);
	TSharedPtr<FStudioAppFeature> SelectedFeature() const;
	bool IsGranted(int64 TierId, const FString& FeatureKey) const;
	int32 GrantCount(const FString& FeatureKey) const;
	bool CanEdit() const;
	// A define, grant or revoke is awaiting its reply, or the lists it would be judged against are still being read.
	bool IsAccessBusy() const;

	TSharedRef<ITableRow> MakeFeatureRow(TSharedPtr<FStudioAppFeature> Feature, const TSharedRef<STableViewBase>& OwnerTable);
	TSharedRef<ITableRow> MakeTierRow(TSharedPtr<FStudioAccessTier> Tier, const TSharedRef<STableViewBase>& OwnerTable);

	TSharedPtr<FCrowdyStudioController> Controller;

	// The app the editors and the selection were made for. Zero means nothing has been loaded for an app yet.
	int64 EditorAppId = 0;

	// Kept by key: every read replaces the controller's list with new records.
	FString SelectedFeatureKey;
	// The key just defined, selected once the re-read brings it in.
	FString PendingSelectKey;
	bool bDefining = false;
	FString Search;

	TArray<TSharedPtr<FStudioAppFeature>> VisibleFeatures;
	TSharedPtr<SListView<TSharedPtr<FStudioAppFeature>>> FeatureListView;
	TSharedPtr<SListView<TSharedPtr<FStudioAccessTier>>> TierListView;
	TSharedPtr<SWidgetSwitcher> BodySwitcher;
	TSharedPtr<SWidgetSwitcher> DetailSwitcher;
	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SEditableTextBox> FeatureKeyBox, FeatureDescBox;
};
