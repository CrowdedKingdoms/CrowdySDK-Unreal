// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyModelCrossLinks.h" // FCrowdyModelLink
#include "GameModel/CrowdyModelLedger.h"
#include "GameModel/CrowdyModelLoadState.h" // ECrowdyModelLoadState
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"

class FCrowdyStudioController;
class SButton;
class SCrowdyModelSectionTable;
class STextBlock;
class SVerticalBox;
class SWidgetSwitcher;

// Whether the host already has this row marked for deletion, so the control on it can read "Unmark" rather than
// offering to mark something twice.
DECLARE_DELEGATE_RetVal_OneParam(bool, FOnCrowdyModelRowIsMarked, const FCrowdyModelRow& /*Row*/);

// Mark or unmark this row. The row travels whole, read from the table at the moment of the click, because a
// destructive control's target has to come from the widget that shows it rather than from anything remembered
// when the control was drawn.
DECLARE_DELEGATE_OneParam(FOnCrowdyModelRowMarkToggled, const FCrowdyModelRow& /*Row*/);

// Follow a cross-link: open this model, show this section, and highlight the row naming this entity. The three
// travel through the call and none of them is a row object or a position, because the table on the other side
// rebuilds every one of its rows whenever anything is read and re-resolves the entity itself.
DECLARE_DELEGATE_ThreeParams(FOnCrowdyModelShowLink,
	const FString& /*TypeName*/, const FString& /*SectionKey*/, const FCrowdyModelRow& /*Entity*/);

// The right-hand pane of the Models browser: what one model is made of, in three sections over the same model.
// Every section renders rows that are already in hand, so moving between them reads nothing. The one server read
// this pane ever issues is the first load of a model's attributes, which cannot be asked for before the model is
// opened; the controller caches that per model, so opening a model a second time issues nothing.
class SCrowdyModelDetailPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyModelDetailPanel) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		SLATE_EVENT(FOnCrowdyModelRowIsMarked, IsRowMarked)
		SLATE_EVENT(FOnCrowdyModelRowMarkToggled, OnToggleRowMark)
		// Left unbound, a cross-link is still readable; bound, it is reachable. Only the host knows how to put
		// another model on screen.
		SLATE_EVENT(FOnCrowdyModelShowLink, OnShowLink)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// Show a model. An empty TypeName is the entry that gathers what belongs to no model: it has no attributes to
	// read, and none are asked for.
	void SetModel(const FCrowdyModelSummary& InModel);

	// Show nothing: either no model is selected, or the one that was has gone from the app.
	void ClearModel();

	// Repaint the three sections from what the controller holds right now. Called when a read lands.
	void RefreshSections();

	// Re-read the highlighted row's delete control. Called when the marked set changes somewhere else, so the
	// control's word follows what is actually marked rather than what it said when it was last drawn.
	void RefreshRowDeleteControl();

	// Switch to a section and highlight the row naming this entity. Returns false when no such row is there, which
	// for the attributes section is what a model whose attributes have not been read yet answers: the caller waits
	// for the read rather than concluding the entity is gone.
	bool ShowSectionRow(const FString& SectionKey, const FCrowdyModelRow& Entity);

private:
	// The index of a section key in the switcher below. Unknown keys map to the first section rather than to
	// nothing, so a stale key can never leave the pane blank.
	static int32 SectionIndex(const FString& SectionKey);
	static FString CountPhrase(int32 Count, const TCHAR* Singular, const TCHAR* Plural);

	// One part of the counts line. A count only when the read that would have filled the table actually landed, and
	// otherwise a phrase naming the state it is really in: a zero over a list nobody read is not a zero, and a read
	// that has failed must never read as one still running. Family is the capitalised name of the read for the
	// phrases that lead with it; Singular and Plural are for the count itself.
	static FString CountPhrase(ECrowdyModelLoadState State, int32 Count,
		const TCHAR* Family, const TCHAR* Singular, const TCHAR* Plural);

	// Which entity a set of cross-links was computed for, as one string, so a repaint can tell a new row from the
	// same row read again. Kind, model and name, each compared as a server key.
	static FString EntityKey(const FCrowdyModelRow& Row);

	FString GetActiveSection() const { return ActiveSection; }
	void SetActiveSection(const FString& SectionKey);
	// The counts line takes all three read states, not just the attributes': the functions and automations tables
	// each show their own placeholder when their read failed, and a header counting them as zero beside it is the
	// same pane answering one question two ways.
	void UpdateHeader(ECrowdyModelLoadState AttributesState,
		ECrowdyModelLoadState FunctionsState, ECrowdyModelLoadState AutomationsState);

	// The table the section tabs currently show. Whatever acts on "the highlighted row" has to ask which table that
	// row would be in, because each section keeps its own selection.
	TSharedPtr<SCrowdyModelSectionTable> ActiveTable() const;

	// Open whatever declares the open model, and whatever declares the highlighted row of the open section. Both are
	// navigation, so neither confirms.
	FReply OnOpenModelClicked();
	FReply OnOpenRowClicked();

	// Whether the row control has something to open. Asked every paint, so the answer for one path is worked out
	// once and kept: the row itself is read fresh each time, and only a change of path costs a lookup.
	bool IsOpenRowEnabled();

	// Mark or unmark the highlighted row. Reads the row from the table at the moment of the click and passes it
	// whole, so nothing between the table and the mark re-types any of its fields.
	FReply OnMarkRowClicked();

	// A table's highlight moved, or its rows were replaced under it. Both have to reach the delete control: a
	// replaced row set drops the highlight, and a control still armed at a row that is no longer on screen is
	// pointed at nothing the reader can see.
	void OnSectionSelectionChanged(TSharedPtr<FCrowdyModelRow> Row, ESelectInfo::Type SelectInfo);

	// Work out what the delete control says and whether it is offered, from the row the active table holds right
	// now. Done here, on the events that can change it, rather than in the control's own bindings: a binding runs
	// on every painted frame and every answer here is a string.
	void UpdateRowDeleteControl();

	// What names the highlighted row, from the app-wide lists already in hand. Runs on the same two events the
	// delete control does, never in a bound attribute: every answer here is a string and a binding would rebuild
	// the whole list on every painted frame. No read is issued: everything this needs was read for the app.
	void UpdateCrossLinks();

	// The line under the section strip for automations that arrived without their triggers. Recomputed on the same
	// events, never bound: it moves only when a read lands or the reader changes section.
	void UpdateTriggerNote();

	FReply OnToggleCrossLinks();

	// Follow one link. The link travels by value and carries the model, the section, the entity name and its kind,
	// which the far side re-resolves against whatever its table holds at that instant. Never a row pointer and
	// never a position: a rebuilt table replaces every row object and drops the vanished one silently.
	FReply OnFollowCrossLink(FCrowdyModelLink Link);

	TSharedPtr<FCrowdyStudioController> Controller;

	FOnCrowdyModelRowIsMarked IsRowMarked;
	FOnCrowdyModelRowMarkToggled ToggleRowMark;
	FOnCrowdyModelShowLink ShowLink;

	// The delete control's state for the highlighted row: whether it may be offered at all, the word on it, and
	// the sentence shown under the section strip when it may not be. All three are set by UpdateRowDeleteControl.
	bool bRowMarkEnabled = false;
	FText RowMarkLabel;
	FText RowMarkTooltip;

	// The model on show, copied rather than pointed at. The list that produced it rebuilds every one of its
	// entries on each refresh, so a pointer into that list outlives the row it came from by design.
	FCrowdyModelSummary Model;
	bool bHasModel = false;

	FString ActiveSection;

	// Whether the open model itself leads anywhere, worked out once when the model is shown rather than on every
	// paint: it can only change when the model does.
	bool bCanOpenModel = false;

	// The last path the row control was asked about and the answer it got. A row's path is read from the table at
	// the moment of the question, so this only ever saves the lookup, never the read.
	FString CheckedRowPath;
	bool bCheckedRowPathOpens = false;

	// The links behind the disclosure, and which entity they were computed for. The open state is reset only when
	// the entity changes: a read landing repaints the lines, and closing the disclosure under a reader who opened
	// it would be a repaint undoing what they did.
	TArray<FCrowdyModelLink> CrossLinks;
	FString CrossLinkEntityKey;
	bool bCrossLinksExpanded = false;

	TSharedPtr<SWidgetSwitcher> RootSwitcher;
	TSharedPtr<SWidgetSwitcher> SectionSwitcher;
	TSharedPtr<STextBlock> MarkRowLabelText;
	// The one line above the cross-links, the control that opens them, and the lines themselves. The summary is
	// phrased by the cross-link layer, because a zero over a list nobody read is not a zero and only that layer is
	// told which lists were read.
	TSharedPtr<STextBlock> CrossLinkSummaryText;
	TSharedPtr<STextBlock> CrossLinkToggleLabel;
	TSharedPtr<SButton> CrossLinkToggleBox;
	TSharedPtr<SVerticalBox> CrossLinkLinesBox;
	// Said under the section strip rather than as a placeholder: the automations themselves rendered, so their
	// table has rows and shows no placeholder at all. Empty, and hidden, unless the triggers actually failed.
	TSharedPtr<STextBlock> TriggerNoteText;
	// Why the highlighted row offers no delete, in one sentence, right where the control would have been. A row
	// the project declares can be deleted and the next sync puts it straight back, so this page offers no such
	// delete at all and says which class or asset to change instead.
	TSharedPtr<STextBlock> RowGateReasonText;
	TSharedPtr<STextBlock> TitleText;
	TSharedPtr<STextBlock> SubtitleText;
	TSharedPtr<STextBlock> DescriptionText;
	TSharedPtr<STextBlock> CountsText;
	TSharedPtr<SCrowdyModelSectionTable> AttributesTable;
	TSharedPtr<SCrowdyModelSectionTable> FunctionsTable;
	TSharedPtr<SCrowdyModelSectionTable> AutomationsTable;
};
