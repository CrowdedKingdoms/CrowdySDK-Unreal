// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyModelLedger.h"
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FActiveTimerHandle;
class FCrowdyStudioController;
class ITableRow;
class SBox;
class SCrowdyDeleteReviewPanel;
class SCrowdyModelDetailPanel;
class SSearchBox;
class STableViewBase;
class STextBlock;

// Models tab: a searchable list of the app's models on the left, and what the selected one is made of on the
// right. Everything shown comes from the ordinary schema reads, so the tab is useful without any plan having
// been run, and it writes nothing.
class SCrowdyModelBrowserTab : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyModelBrowserTab) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyModelBrowserTab() override;

	// Opens the given model exactly as clicking its row would: selects it, scrolls it into view and loads its
	// sections into the detail pane, reusing the same selection path a click uses. If this browser does not
	// currently list the type -- pruned from the app between reads -- the current selection is left untouched;
	// this never clears the pane.
	void SelectModel(const FString& TypeName);

	// SelectModel, then the section on it, then the row naming this entity. Everything SelectModel already does is
	// reused: the pending-refresh flush, the "not in this list" early out, the search that clears if it hides the
	// row, and the one-tick deferred scroll. An empty SectionKey is a plain SelectModel, which is what a caller
	// naming a model and nothing on it means.
	//
	// The entity is re-resolved against whatever the table holds when it lands, never carried as a row object or a
	// position: a rebuild replaces every row and drops the vanished one silently. Landing on an attribute of a
	// model nobody has opened waits for the read that opening it issues, and gives up if that read fails.
	void SelectModelRow(const FString& TypeName, const FString& SectionKey, const FCrowdyModelRow& Entity);

private:
	static FString CountPhrase(int32 Count, const TCHAR* Singular, const TCHAR* Plural);

	void HandleAppChanged();
	void HandleContainerTypesChanged();
	void HandleFunctionsChanged();
	void HandleAutomationsChanged();
	void HandlePropertyDefsChanged();
	// A plan finished, or the selection moved to an app with a different one. Every row's source and status is a
	// statement about that plan, so all of them are rebuilt.
	void HandleModelSnapshotChanged();

	// Notice that this tab's contents describe an app other than the selected one, and empty them if so. Called
	// both from the app-changed announcement and from the model-list refresh the switch itself triggers, because
	// the announcement waits on the new app's token being minted and a mint can fail, leaving this tab stale with
	// no notification at all. Comparing the app id makes running it twice a no-op.
	void SyncEditorAppScope();
	// Empty everything this tab holds for itself: the search text, the selected model and the sections under it.
	void ClearAppScopedEditors();

	// A controller delegate marks what went out of date and a single pass brings it back. Five delegates arrive
	// for one Refresh, and rebuilding on each of them would build the same row set five times over.
	void MarkLedgerDirty();
	void MarkFilterDirty();
	void MarkDetailDirty();
	void ScheduleRefresh();
	EActiveTimerReturnType HandleScheduledRefresh(double InCurrentTime, float InDeltaTime);
	// Bring the list and the pane up to date right now. The scheduled pass runs from an active timer, which only
	// runs while this tab is being painted, so anything reading the model list from outside the paint path has to
	// settle what is owed itself rather than wait for a timer that a hidden tab never reaches.
	void FlushPendingRefresh();

	// SelectModel's scroll, deferred one tick: a widget in a collapsed SWidgetSwitcher slot has no geometry, so a
	// scroll-into-view issued in the same call as the tab switch that reveals it can land before the list has any
	// to scroll within.
	EActiveTimerReturnType HandleScrollToSelection(double InCurrentTime, float InDeltaTime);
	TWeakPtr<FActiveTimerHandle> ScrollTimerHandle;

	void RebuildLedger();
	// bModelsRebuilt says whether the summaries themselves were rebuilt, as opposed to the same summaries merely
	// being filtered down. A filter changes which models are listed, never what any of them is made of, so it must
	// not push the open model back into the detail pane: that would rebuild all three of its tables and drop the row
	// the user had highlighted, on every pause in typing.
	void ApplyFilter(bool bModelsRebuilt);
	void RestoreSelection(bool bModelsRebuilt);
	void ShowInDetailPanel(const FCrowdyModelSummary& Summary);
	void ClearDetailPanel();
	void UpdateListPlaceholder();
	// The one line saying how old the source and status columns are. Rebuilt when the plan behind them changes,
	// never in a text binding: an absolute time does not move between frames, and a phrase that did would be
	// rebuilt on every one of them.
	void UpdateFreshnessLine();

	// The source setting, stored as the key the segmented control reports and read back as the filter the pure
	// ledger takes. The control is a strip of keys, and mapping in one place keeps an unrecognized key from
	// silently meaning "everything".
	static ECrowdyModelSourceFilter FilterForKey(const FString& Key);
	FString GetSourceFilterKey() const { return SourceFilterKey; }
	void OnSourceFilterSelected(const FString& Key);

	void OnSearchTextChanged(const FText& NewText);
	void OnModelSelectionChanged(TSharedPtr<FCrowdyModelSummary> Model, ESelectInfo::Type SelectInfo);
	TSharedRef<ITableRow> MakeModelRow(TSharedPtr<FCrowdyModelSummary> Model, const TSharedRef<STableViewBase>& OwnerTable);

	// Mark or unmark the highlighted model. The target is read from the list view at the moment of the click and
	// handed over whole: a remembered model outlives the row it names, because every refresh replaces the row set,
	// and an app-identity check cannot catch that because what fails there is target identity.
	FReply OnMarkModelClicked();
	// Open the delete review over whatever is marked. Nothing has been read until this is pressed.
	FReply OnReviewDeletionsClicked();

	// What the model list's delete control says and whether it is offered at all, worked out from the list's live
	// selection when something moves rather than in the control's own bindings: a binding runs on every painted
	// frame and every answer here is a string.
	void UpdateModelDeleteControl();

	// The delete panel's questions about a section row, answered from the one marked set.
	bool IsRowMarkedForDelete(const FCrowdyModelRow& Row) const;
	void ToggleRowDeleteMark(const FCrowdyModelRow& Row);

	// The marked set changed size. Shows or hides the review beside the list and re-reads both delete controls, so
	// a row marked from one place reads as marked everywhere.
	void HandleMarksChanged(int32 MarkCount);
	// A commit ended, finished or stopped. The controller has already re-read what it changed, so this only
	// rebuilds what is on screen.
	void HandleDeleteCommitFinished();
	// A blocker in the review was followed. Opens the model it named, which is where the rows blocking it are.
	void HandleShowBlockingModel(const FString& TypeName);
	// A cross-link in the detail pane was followed.
	void HandleShowCrossLink(const FString& TypeName, const FString& SectionKey, const FCrowdyModelRow& Entity);

	// Land a navigation that had to wait for a read. Runs after every refresh pass, and gives up as soon as there
	// is nothing left to wait for: a read that failed leaves the placeholder saying so, and holding the target open
	// past that would yank the reader back to a section they had moved away from.
	void TryCompletePendingRow();

	TSharedPtr<FCrowdyStudioController> Controller;

	// The app this tab's contents were read for. Zero means nothing has been loaded for an app yet.
	int64 EditorAppId = 0;

	// Every model of the app, and the ones the search box leaves. The list view renders the filtered set; the
	// full set answers whether a model that was open still exists.
	TArray<FCrowdyModelSummary> AllModels;
	TArray<TSharedPtr<FCrowdyModelSummary>> FilteredModels;
	FString SearchQuery;

	// Which sources the list shows. It composes with the search box rather than replacing it: a model has to
	// satisfy both.
	FString SourceFilterKey;

	// The open model's type name rather than its row. A rebuild replaces every row object, so the row that was
	// selected simply stops existing, while a name can be looked up again in whatever the list holds now. Unset
	// is nothing selected; set and empty is the entry for what belongs to no model.
	TOptional<FString> SelectedTypeName;

	// The model the detail pane is currently showing. Kept apart from the selection above so that re-placing the
	// list highlight can tell whether the pane already holds this model and be left alone if it does.
	TOptional<FString> ShownTypeName;

	// A navigation waiting on a read. Held by name and by entity rather than by row, because the rows it is waiting
	// for do not exist yet and the ones that do will all have been replaced by the time it lands.
	bool bHasPendingRow = false;
	FString PendingRowModel;
	FString PendingRowSection;
	FCrowdyModelRow PendingRowEntity;

	bool bLedgerDirty = false;
	bool bFilterDirty = false;
	bool bDetailDirty = false;
	TWeakPtr<FActiveTimerHandle> RefreshTimerHandle;

	// The model list's delete control: whether it is offered, the word on it, and why not when it is refused.
	bool bModelMarkEnabled = false;
	FText ModelMarkLabel;
	FText ModelMarkTooltip;

	TSharedPtr<SSearchBox> SearchBox;
	TSharedPtr<SListView<TSharedPtr<FCrowdyModelSummary>>> ModelListView;
	TSharedPtr<STextBlock> ModelListPlaceholderText;
	TSharedPtr<STextBlock> FreshnessText;
	TSharedPtr<STextBlock> MarkModelLabelText;
	// Why the highlighted model offers no delete, said where the control would have been. A model the project
	// declares can be deleted and the next sync puts it straight back, so this page offers no such delete at all.
	TSharedPtr<STextBlock> ModelGateReasonText;
	TSharedPtr<STextBlock> ReviewButtonLabelText;
	TSharedPtr<SBox> ReviewButtonBox;
	TSharedPtr<SCrowdyModelDetailPanel> DetailPanel;

	// The delete review, and the slot it lives in. The slot is collapsed while nothing is marked, so a page nobody
	// is deleting from keeps its whole width for the list and the model on it.
	TSharedPtr<SCrowdyDeleteReviewPanel> DeletePanel;
	TSharedPtr<SBox> DeletePanelBox;
};
