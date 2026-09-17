#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;
class SComboButton;
class SMenuAnchor;
template <typename OptionType> class SComboBox;

// The Game Model page's second card: create the server rows a map's level-placed entities will bind before anyone
// plays. Scan open map writes the map's manifest; Preview reads the server for the chosen scope and diffs; Apply
// opens the review, which is the one confirmation, and creates only what the review shows. Nothing here deletes.
class SCrowdyPreSeedCard : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyPreSeedCard) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyPreSeedCard() override;

private:
	FReply OnScanClicked();
	FReply OnPreviewClicked();
	FReply OnApplyClicked();
	TSharedRef<SWidget> MakeReviewPanel();
	TSharedRef<SWidget> MakeDetailsPanel();
	// Refreshes the cached lines; bound to the controller's report broadcast so nothing is formatted per paint.
	void HandleReportChanged();
	// An app switch retires the other app's sessions from the picker.
	void HandleAppChanged();
	void RefreshScopes();
	FText GetManifestLineText() const { return CachedManifestLine; }
	FText GetCountLineText() const { return CachedCountLine; }
	FText GetReportText() const { return CachedReportText; }
	FText GetScopeLabel() const;

	TSharedPtr<FCrowdyStudioController> Controller;
	// The scope options: the app entry first (empty session id), then the app's sessions as last fetched.
	TArray<TSharedPtr<FStudioSession>> Scopes;
	TSharedPtr<FStudioSession> SelectedScope;
	TSharedPtr<SComboBox<TSharedPtr<FStudioSession>>> ScopeCombo;
	TSharedPtr<SMenuAnchor> ApplyAnchor;
	TSharedPtr<SComboButton> DetailsButton;
	FText CachedManifestLine;
	FText CachedCountLine;
	FText CachedReportText;
};

// The review over the page: the plan's rows and the one button that creates the missing ones. Opened on a plan
// generation and refuses a plan that moved since.
class SCrowdyPreSeedReviewPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyPreSeedReviewPanel) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		SLATE_EVENT(FSimpleDelegate, OnDismiss)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	FReply OnCreateClicked();
	FReply OnCancelClicked();
	FText GetCreateButtonLabel() const;
	bool CanCreate() const;

	TSharedPtr<FCrowdyStudioController> Controller;
	FSimpleDelegate OnDismiss;
	uint64 GenerationSeen = 0;
};
