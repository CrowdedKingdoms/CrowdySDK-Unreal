// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyModelLedger.h" // FCrowdyModelRow
#include "Types/SlateEnums.h"
#include "UI/CrowdyStudioWidgets.h" // CrowdyStudioWidgets::FCrowdyTabItem
#include "Widgets/SCompoundWidget.h"

class FActiveTimerHandle;
class FCrowdyStudioController;
class SCrowdyModelBrowserTab;
class SWidgetSwitcher;

// Game Model page shell: the page title and its refresh, the schema reconcile strip, and a tab strip
// over the model browser, the live instance browser and the advanced editors. It holds no app-scoped
// state; each tab reads the controller and listens for itself. The one thing the shell listens for is
// the app changing, so the page can load itself the next time it is shown.
class SCrowdyGameModelView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyGameModelView) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyGameModelView() override;

private:
	// Page-level refresh of the schema reads every tab shares. The Live tab refreshes its own list.
	FReply OnRefreshClicked();
	FReply OnLintClicked();

	// Ask the controller for this app's schema lists, once, the next time this page is painted. Every Studio page is
	// built up front into one switcher, so this page exists from startup whether or not anyone opens it: doing the
	// reads on construction would load the game-model schema for a user who never comes here. An active timer only
	// runs while its widget is being painted, and a widget in an inactive switcher slot never is, so registering one
	// is exactly "when this page is actually on screen".
	void ScheduleEnsureLists();
	EActiveTimerReturnType HandleEnsureLists(double InCurrentTime, float InDeltaTime);
	TWeakPtr<FActiveTimerHandle> EnsureListsTimerHandle;

	// The app changed, so whatever this page holds belongs to the previous one. Arm the load again rather than
	// issuing it here: the announcement arrives whether or not this page is the one on screen.
	void HandleAppChanged();

	// A plan started, moved on, or ended. Every part of a plan is asynchronous, so without an indicator the page
	// looks idle from the click until the report lands. Updated here, on the announcement, rather than from a bound
	// attribute: text bindings run on every painted frame and none of this changes between them.
	void HandleSchemaPlanProgress();
	TSharedPtr<class SWidget> PlanProgressRow;
	TSharedPtr<class STextBlock> PlanProgressText;

	// Tabs are addressed by a stored key rather than an index, so inserting or reordering a tab can
	// never re-point a persisted value at a different one. INDEX_NONE for an unrecognized key.
	static int32 TabIndexForKey(const FString& TabKey);
	void OnTabSelected(const FString& TabKey);

	// The tab strip, rebuilt as data so the one conditional tab states its own availability and hint
	// rather than the strip carrying a second set of parallel arrays about it.
	TArray<CrowdyStudioWidgets::FCrowdyTabItem> MakeTabItems();

	// Whether the Issues tab has anything to show. Findings, not errors: `clean` ignores warnings.
	bool HasModelIssues() const;
	// Every tab but Issues is always available; Issues follows the findings.
	bool IsTabAvailable(const FString& TabKey) const;
	// What the Issues tab says on hover, which is a different sentence for never-checked and checked-clean.
	FText IssuesTabToolTip() const;

	// A lint landed. Only one thing here reacts: leaving the Issues tab when it just became unavailable.
	void HandleLintChanged();

	// The Live tab's "Show in Models" cross-link. Switches to the Models tab, then hands it the type name to
	// open. Cross-tab navigation is the shell's job; the type name travels through the call and is not kept.
	void OnShowModelInBrowser(const FString& TypeName);

	// The same, aimed at one row rather than at a model. Switching to the Models tab comes first for the same
	// reason it always has: a collapsed switcher slot has no geometry, and a scroll issued before the switch has
	// nothing to scroll within. An empty section key opens the model and nothing on it.
	void ShowModelRowInBrowser(const FString& TypeName, const FString& SectionKey, const FCrowdyModelRow& Entity);

	TSharedPtr<FCrowdyStudioController> Controller;
	TSharedPtr<SWidgetSwitcher> TabSwitcher;

	// Held so the cross-tab link above can reach the Models tab directly, without routing a second delegate
	// through the controller for what is purely a page-navigation concern.
	TSharedPtr<SCrowdyModelBrowserTab> ModelBrowserTab;

	// The open tab's key, seeded from the persisted value and falling back to the first tab when that
	// value is empty or names a tab that no longer exists.
	FString ActiveTab;
};
