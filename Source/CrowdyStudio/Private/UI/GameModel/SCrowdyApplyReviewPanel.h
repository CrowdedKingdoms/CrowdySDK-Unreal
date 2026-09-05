// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyApplySelection.h"
#include "Styling/SlateTypes.h" // ECheckBoxState
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class ITableRow;
class SButton;
class STableViewBase;
class STextBlock;
class SVerticalBox;

/** One line of the review list: a group heading, or one entity with a tick beside it. */
struct FCrowdyApplyReviewRow
{
	// A heading carries only its text and is never ticked. Headings ride in the same list as the entities under
	// them so the whole list stays virtualized: a model with two hundred attributes costs what one with two does.
	bool bHeading = false;
	FString Heading;

	FCrowdyApplyUnit Unit;

	// Built once, when the row is built. Everything that addresses this entity compares this string, and it is
	// case-sensitive, because two server keys differing only in case are two entities.
	FString IdentityKey;

	bool bChecked = true;

	// Whether this entity may be ticked at all, and the sentence shown beside it when it may not.
	bool bSelectable = true;
	FString Reason;

	// Whether the plan creates this entity or updates one the server already has, in one word.
	FString StateWord;
};

// One row's tick was clicked. The row travels whole rather than as an index: the panel rebuilds its item set
// whenever the pending plan moves, and an index into the previous set names a different entity afterwards.
DECLARE_DELEGATE_OneParam(FOnCrowdyApplyRowToggled, TSharedPtr<FCrowdyApplyReviewRow>);

/**
 * Pick what to send, see what that drags in with it, send once.
 *
 * The review surface for a pending schema plan. Everything is ticked when it opens, so a reader who changes
 * nothing sends exactly what the all-or-nothing sync sent. Unticking is per entity: never per family and never
 * per model, because the reason to hold something back is always about one entity. Whatever a selection needs is
 * added back automatically and said out loud, with the entity that forced each addition named, so the set that
 * actually goes to the server is the set on screen.
 *
 * IT DECIDES NOTHING. Which entities exist, which of them may be ticked, what a selection drags in, in what
 * order it runs, what the sheet says and whether it may be sent at all are all answered by the selection layer.
 * This renders those answers and re-asks for them; it never re-derives one, so the rule a test exercises and the
 * rule a reader sees cannot drift apart.
 *
 * FLOATS OVER THE PAGE, deliberately. The strip it opens from is fixed-height content above a tab area that does
 * not scroll, so anything given a permanent place there is taken from the browser below for good, and anything
 * that grows pushes the page off the bottom. This appears when it is asked for and costs nothing the rest of the
 * time.
 *
 * TWO THINGS A MESSAGE DIALOG GAVE FOR FREE and that are re-established here explicitly, both load-bearing:
 *
 *   1. The send button is NOT a focus target and does NOT answer Enter. It is built non-focusable and nothing
 *      here ever moves focus to it. A page whose write control is one keypress from a box the reader was typing
 *      in has no confirmation at all.
 *   2. The sheet is STICKY. It sits outside the scrolling half, so the action and the count it names can never be
 *      separated on screen: a button reading "Send 47 changes" is a confirmation only while the 47 is beside it.
 *
 * AND ONE THING NO DIALOG EVER HAD. The send re-gathers the pending plan FROM THE CONTROLLER at the moment of
 * the click and compares a freshly built consent token against the one the sheet was drawn with. A read landing
 * between the sheet being drawn and the button being pressed changes that token and the send is refused. A guard
 * that rebuilt from the plan already held here would re-derive the same answer and agree with itself however far
 * the server had moved.
 */
class SCrowdyApplyReviewPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyApplyReviewPanel) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		// Asks the host to take this panel off the page. Fired by the close control and once a send has been
		// issued; never fired by anything that only changed what is on screen.
		SLATE_EVENT(FSimpleDelegate, OnDismiss)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyApplyReviewPanel() override;

private:
	// Gather the pending plan from the controller and rebuild everything from it: the list, the additions, the
	// sheet and the consent token the send is judged against. The plan input holds the controller's arrays BY
	// POINTER and is valid only until something re-plans, so it is gathered, used and dropped inside one call and
	// never kept.
	//
	// bAnnounceReplan is set by the one caller that runs because the pending plan was REPLACED under an open sheet.
	// This rebuild refreshes the consent token, which re-arms the send control, so without a word said the reader
	// is offered a live write against a sheet whose contents changed while they were reading it. Callers that say
	// so themselves in stronger words pass false.
	void RebuildFromController(bool bAnnounceReplan);

	// Rebuild only what a tick can change: the closure, the additions and the sheet. The list of entities is the
	// plan's and does not move when the selection does.
	void RefreshPlanFromSelection();

	void RebuildUnitRows(const TArray<FCrowdyApplyUnit>& Selectable);
	void RebuildAdditions();
	void RebuildSheet();

	// The keys this press would send. Everything ticked means the WHOLE plan, wiring included, rather than every
	// tickable row: the list hides the wiring the SDK provisions itself, so a model whose only pending changes are
	// that wiring has no row to tick and taking the ticked rows here would leave it permanently unsent.
	TArray<FString> GatherSelectionKeys(const FCrowdyApplyPlanInput& Input) const;

	// Refresh SelectedKeys from the rows that are ticked right now, and notice when that is all of them: a reader
	// who unticked one entity and ticked it again is back to sending everything, and the two must be one state.
	void CaptureSelectionFromRows();

	void HandleRowToggled(TSharedPtr<FCrowdyApplyReviewRow> Row);
	FReply OnSelectAllClicked();
	FReply OnClearAllClicked();
	FReply OnCloseClicked();

	// Whether the send control is armed. The plan's own verdict, so a refusal cannot be lost by a widget that
	// forgot to ask; the controller checks it again, so this is a second statement of the rule rather than the
	// only one.
	bool IsSendEnabled() const;
	FReply OnSendClicked();

	TSharedRef<ITableRow> MakeUnitRow(TSharedPtr<FCrowdyApplyReviewRow> Row, const TSharedRef<STableViewBase>& OwnerTable);

	// One sentence about something that happened away from the sheet: a plan recomputed under it, or a selection
	// that lost entries because of it. An empty message hides the line rather than leaving a blank row behind.
	void SetNotice(const FText& Message);

	// The pending plan was replaced, or the app moved. Both make what is on screen a description of something
	// else.
	void HandleSchemaSyncReportChanged();
	void HandleAppChanged();

	TSharedPtr<FCrowdyStudioController> Controller;

	FSimpleDelegate Dismiss;

	// The app this panel believes it is writing to: the app selected as it opened. Carried into the send so the
	// controller compares three sources rather than one value agreeing with itself, this being the widget's own
	// answer while the plan built at click time carries the app the pending arrays are pinned to. Deliberately not
	// the planned app, which is zero while a plan is still running and would refuse every later send.
	int64 ExpectedAppId = 0;

	// The plan the sheet on screen describes.
	FCrowdyApplyPlan Plan;

	// The plan's token as the sheet was last drawn. Compared against a freshly built one at the moment of the
	// click; anything that moved in between changes it and the send is refused.
	FString ShownConsentToken;

	// Whether this press sends the whole plan. True while nothing has been unticked, which is what makes an apply
	// nobody narrowed identical to the all-or-nothing one.
	bool bSelectionIsAll = true;

	// What is ticked, by identity rather than by position, so a selection survives the pending arrays being
	// rebuilt from a fresh diff. Meaningless while bSelectionIsAll is set.
	TArray<FString> SelectedKeys;

	TArray<TSharedPtr<FCrowdyApplyReviewRow>> UnitRows;

	TSharedPtr<SListView<TSharedPtr<FCrowdyApplyReviewRow>>> UnitListView;
	TSharedPtr<SVerticalBox> AdditionsBox;
	TSharedPtr<SVerticalBox> UnresolvableBox;
	TSharedPtr<STextBlock> AdditionsHeaderText;
	TSharedPtr<STextBlock> NoticeText;
	TSharedPtr<STextBlock> HeadlineText;
	TSharedPtr<STextBlock> AppLineText;
	TSharedPtr<STextBlock> CountsText;
	TSharedPtr<STextBlock> BlockedReasonText;

	// The send control's own label, set when the sheet is rebuilt rather than bound: a bound text attribute
	// builds its string on every painted frame and this one moves only when the plan does.
	TSharedPtr<STextBlock> SendLabelText;

	// Built non-focusable, and nothing here ever moves keyboard focus to it, so Enter pressed anywhere on the page
	// cannot fire it.
	TSharedPtr<SButton> SendButton;
};
