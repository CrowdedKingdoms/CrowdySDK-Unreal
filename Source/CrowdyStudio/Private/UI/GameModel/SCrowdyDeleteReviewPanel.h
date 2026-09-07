// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyGameModelDelete.h"
#include "Styling/SlateTypes.h" // ECheckBoxState
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class FCrowdyStudioController;
class ITableRow;
class SButton;
class SCheckBox;
class STableViewBase;
class STextBlock;
class SVerticalBox;
class SWidgetSwitcher;

// Reports how many entities are marked for deletion, so the tab hosting this panel can label its own control
// without reaching in for the list.
DECLARE_DELEGATE_OneParam(FOnDeleteMarksChanged, int32 /*MarkCount*/);

// Asks the host to open one model, so a finding that names what is stopping a delete can be followed rather than
// merely read. Every blocker this panel can raise is about a model, and the host is the only widget that knows how
// to put one on screen.
DECLARE_DELEGATE_OneParam(FOnDeleteReviewShowModel, const FString& /*TypeName*/);

/**
 * Mark, review, commit once.
 *
 * The delete surface for the Models browser. Marking costs nothing and reads nothing; opening the review is the
 * one moment anything is read, and it reads once for the whole marked set rather than once per mark, so a model
 * with a million live instances cannot turn a pre-flight into a stall. What the review then shows is a plan
 * from the pure delete layer: what will run, in what order, what it costs, and a sheet whose confirmation is
 * scaled to the worst finding in it.
 *
 * NOT A MODAL, on purpose. The confirms this needs are all outside what a message dialog can express: a custom
 * destructive verb, a primary button that stays disabled, clickable blocker lines, and a row to tick. Two
 * things a modal gave for free have to be re-established here explicitly, and both are load-bearing rather than
 * cosmetic:
 *
 *   1. The delete button is NOT the default focus target and does NOT answer Enter. It is built non-focusable
 *      and nothing ever moves focus to it. A page whose most destructive control is one keypress from a text
 *      box the reader was typing in has no confirmation at all.
 *   2. The sheet is STICKY. It sits outside the scrolling half, so the action and the count it names can never
 *      be separated on screen: a button reading "Delete 47 entries" is only a confirmation while the 47 is
 *      still visible beside it.
 *
 * WHAT THE HOST DRIVES. The Models browser tab owns one of these and does four things with it: it marks a row
 * the reader chose, it unmarks one, it asks whether a row is already marked so the row's control can read
 * "Unmark", and it opens the review. The panel owns the marked set itself, because a mark outlives the section
 * switch, the model switch and the search query that produced it, and because one owner is the only way the
 * sheet's counts and the list of marks can be the same answer.
 *
 * WHERE A MARK COMES FROM. Never from anything this panel remembers. The host reads its list view's live
 * selection count at the moment of the click, reads the selected row from the same widget in the same
 * statement, and passes the row whole to CrowdyGameModelDelete::MarkFromRow. A remembered row outlives the row
 * it names: replacing a table's rows builds a whole new set of objects and drops the vanished one silently, so
 * a cached target keeps pointing at something that is no longer on screen. An app-identity check cannot catch
 * that, because what has gone wrong is target identity, not app identity.
 *
 * WHAT A ROW MAY OFFER AT ALL. The host asks CrowdyGameModelDelete::CanDeleteFromPrimaryView before it offers
 * anything. A row the project declares gets no delete control here: the delete would succeed and the next sync
 * would put the entity straight back, which is work undoing itself with a success message in between. The row
 * says why and offers an Open instead. The escape hatch with its consequence written on the button lives on
 * the Advanced tab, which exists to offer everything the server allows.
 */
class SCrowdyDeleteReviewPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCrowdyDeleteReviewPanel) {}
		SLATE_ARGUMENT(TSharedPtr<FCrowdyStudioController>, Controller)
		// Fires whenever the marked set changes, with its new size, so the host can label its own control and
		// show or hide whatever opens this panel.
		SLATE_EVENT(FOnDeleteMarksChanged, OnMarksChanged)
		// Fires once a commit has ended and the controller has re-read what it changed, so the host can rebuild
		// its rows. Fires for a commit that stopped as well as one that finished: a partial commit changed the
		// server too, and a list still showing what it deleted is the more misleading of the two.
		SLATE_EVENT(FSimpleDelegate, OnCommitFinished)
		// Fires when a blocker line is followed, with the model that blocker is about. Left unbound, a blocker is
		// still readable; bound, it is reachable.
		SLATE_EVENT(FOnDeleteReviewShowModel, OnShowModel)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCrowdyDeleteReviewPanel() override;

	// Marking. None of these read a server, load an asset or build a plan: the pre-flight runs when the review
	// opens, not per mark, so ticking a hundred rows costs a hundred array appends and nothing else.
	//
	// Marking an entity already marked is a no-op rather than a duplicate, so a host that cannot tell (a row
	// rebuilt under it, a keyboard repeat) is never punished for asking twice.
	void MarkForDelete(const FCrowdyDeleteMark& Mark);
	void UnmarkForDelete(const FCrowdyDeleteMark& Mark);
	bool IsMarked(const FCrowdyDeleteMark& Mark) const;
	int32 NumMarked() const { return Marks.Num(); }

	// Drop every mark. Also what an app switch does, since a mark names an entity in one app and means nothing
	// in another.
	void ClearMarks();

	// Mark every entity in this app the project does not declare. This is what the old server-only prune
	// becomes: a way to mark a lot at once, in front of the same sheet and the same ladder as a single row,
	// rather than a bulk delete standing beside a write control. It marks; it never commits.
	void MarkEverythingServerOnly();

	// Whether that bulk mark may be offered, and why not when it may not. Refused with no plan captured, since
	// server-only is a classification and without a plan there is none; and refused on any app a Game Kit has
	// been deployed to, because only kit functions carry prune protection today, so a bulk mark there would
	// offer the kit's own models and attributes for deletion. Asked every paint, and cheap for that reason.
	bool CanMarkEverythingServerOnly(FString& OutReason) const;

	// Mark every entity this app holds on the server, code-backed and kit-owned included. Marks; never commits.
	void MarkEverythingOnServer();

	bool CanMarkEverythingOnServer(FString& OutReason) const;

	// Open the review over whatever is marked right now. This is the one place this surface reads anything: it
	// gathers what the controller already holds, then issues the bounded live-model probe for the marked models,
	// once for the whole set. The sheet appears when that completes.
	void OpenReview();

	// Close it and go back to the marked list. The marks survive: closing is not cancelling.
	void CloseReview();

	bool IsReviewOpen() const { return bReviewOpen; }

private:
	// The three faces of this panel: nothing marked, a marked list with a control to review it, and the review
	// itself. Named rather than numbered, so a slot added between two of them cannot silently re-point the
	// others.
	enum class EPage : int32
	{
		Empty = 0,
		Marked = 1,
		Review = 2
	};

	void ShowPage(EPage Page);

	// Gather everything the plan is judged against from what the controller already holds. No read is issued: the
	// one read this surface makes is the live-model probe. The probe's counts are CARRIED FORWARD from whatever is
	// already held rather than dropped, for the same reason the captured plan is: a gather that emptied them would
	// turn every marked model Unknown, which is a blocker, so the remainder of a stopped commit could never be
	// finished from the screen that tells the reader to finish it.
	FCrowdyDeleteEvidence GatherEvidence() const;

	// Re-gather into the held evidence. Runs whenever the marked set moves and whenever a controller read lands,
	// not only while the review is open: the marked list names entities too, and what may be said about a name
	// depends on the same lists a plan is judged against.
	void RefreshEvidence();

	// The pre-flight's completion. Folds the counts into the held evidence and builds the plan, or explains why
	// it will not: a completion for an app this panel is no longer showing is dropped, and the review closes
	// saying so rather than rendering a sheet about somewhere else.
	void HandlePreflightCounts(int64 InAppId, TArray<FCrowdyDeleteLiveCount>&& Counts);

	// Rebuild the plan from the evidence in hand and repaint the sheet, the findings and the marked list. Pure
	// and cheap, so it also runs when a controller read lands while the review is open: the rows behind a
	// finding can change under an open sheet, and a sheet that does not follow them is a promise about a server
	// that has moved.
	void RebuildPlan();

	void RebuildMarkedList();
	void RebuildFindings();
	void RebuildSheet();

	// One finding as a block: its severity, its one sentence, what to do about it, and a disclosure holding every
	// reference behind it. The disclosure's open state lives in the widgets it switches rather than in a member,
	// so a rebuilt list starts from the same place instead of from a flag that outlived the finding it described.
	TSharedRef<SWidget> MakeFindingBlock(const FCrowdyDeleteFinding& Finding);

	// Follow a blocker to the model it is about. Navigation, so it confirms nothing; the model travels through the
	// call and is not kept.
	FReply OnShowBlockingModelClicked(FString TypeName);

	// The count above the list, and whether the bulk mark may be offered. Both are recomputed when something moves
	// rather than read from a bound attribute, because a bound attribute is evaluated on every painted frame and
	// neither of these can change between two of them.
	void UpdateHeader();
	void UpdateBulkMarkAffordance();

	// One sentence about something that happened away from the sheet: a check that came back for another app, or a
	// commit that has just ended. Shown beside the marked list, where the reader is left standing afterwards. An
	// empty message hides the line rather than leaving a blank row behind.
	void SetNotice(const FText& Message);

	// Show the review with its sheet disabled while the live-model check is out. The sheet cannot say what a delete
	// costs until the counts land, and a sheet that filled itself in afterwards would have been read before it was
	// true.
	void ShowPreflightPending();

	// Arm or disarm the commit control from the plan on screen. The button is enabled only when the plan is
	// committable AND, where the ladder asks for one, the acknowledgement is ticked.
	bool IsCommitEnabled() const;
	FReply OnCommitClicked();

	// Remove one mark from the marked list. Reads the highlighted row from the list view at the moment of the
	// click, exactly as the host does, and does nothing at all when nothing is highlighted.
	FReply OnUnmarkSelectedClicked();
	FReply OnClearMarksClicked();
	FReply OnReviewClicked();
	FReply OnCloseReviewClicked();

	void OnAcknowledgeChanged(ECheckBoxState NewState);
	ECheckBoxState GetAcknowledgeState() const;

	TSharedRef<ITableRow> MakeMarkRow(TSharedPtr<FCrowdyDeleteMark> Mark, const TSharedRef<STableViewBase>& OwnerTable);

	// Controller signals.
	void HandleAppChanged();
	// A schema read landed. The plan on screen was computed against the lists these replace, so it is rebuilt.
	void HandleSchemaListsChanged();
	void HandleModelSnapshotChanged();
	void HandleCommitProgress();
	void HandleCommitFinished();

	// Notice that what this panel holds belongs to an app other than the selected one, and empty it if so.
	// Called from the app-changed announcement and from the read refreshes the switch itself triggers, because
	// the announcement waits on the new app's token being minted and a mint can fail, which would leave marks
	// for the previous app sitting here with no notification ever made.
	void SyncEditorAppScope();

	// Every change to the marked set goes through here: the list, the header, the bulk affordance, the sheet if one
	// is open, and finally the host. One path, so the count on the host's control and the count on the sheet cannot
	// come from two different answers.
	void NotifyMarksChanged();

	TSharedPtr<FCrowdyStudioController> Controller;

	FOnDeleteMarksChanged MarksChanged;
	FSimpleDelegate CommitFinished;
	FOnDeleteReviewShowModel ShowModel;

	// The app every mark below belongs to. An attribute key and a function name usually carry the SAME spelling
	// in a development and a production app, since both are deployed from the same project, so a delete has to
	// be aimed at the app its subject was read from rather than at whatever is selected when the button is
	// pressed. Zero means nothing is marked for any app.
	int64 EditorAppId = 0;

	// The marked set, in the order it was marked, which is the order the marked list shows. The commit order is
	// the plan's and is not this one.
	TArray<FCrowdyDeleteMark> Marks;

	// The marked set as list items. Rebuilt whenever Marks changes, because a list view holds its items by
	// shared pointer and compares its selection by pointer identity.
	TArray<TSharedPtr<FCrowdyDeleteMark>> MarkItems;

	bool bReviewOpen = false;

	// The probe is out. A second Review press while it is set is refused rather than stacking a second set of
	// reads over the same models.
	bool bPreflightInFlight = false;
	// The app the probe was issued for. A completion for any other app is dropped: it describes somewhere the
	// reader is no longer looking, and its counts would clear or raise a blocker about the wrong models.
	int64 PreflightAppId = 0;

	// Everything the plan is judged against, held for the life of the review rather than re-gathered per
	// rebuild. It holds the entities by shared pointer, so the rows behind an open sheet stay alive even after
	// the controller has replaced the arrays they came from.
	FCrowdyDeleteEvidence Evidence;

	// The plan the sheet on screen describes.
	FCrowdyDeletePlan Plan;

	// The plan's token as the sheet was last drawn. Compared against a freshly built plan's token at the moment
	// the commit button is pressed: anything that moved between the sheet being drawn and the button being
	// pressed changes it, and the commit is refused rather than running against consent given for a different
	// set. A guard that compared the plan to itself would agree however stale the sheet had become.
	FString ShownConsentToken;

	// Whether the acknowledgement has been ticked. Reset by every rebuild of the plan, because an
	// acknowledgement is consent to the cautions that were on screen when it was ticked and not to whatever
	// replaced them.
	bool bAcknowledged = false;

	// Whether the bulk mark may be offered and the sentence to show when it may not, worked out when the app, the
	// plan or the marked set moves rather than while the button is painted.
	bool bBulkMarkAllowed = false;
	FText BulkMarkReason;

	// The same pair for the unconditional mark, which is allowed in cases the server-only one refuses.
	bool bPurgeMarkAllowed = false;
	FText PurgeMarkReason;

	TSharedPtr<SWidgetSwitcher> PageSwitcher;
	TSharedPtr<SListView<TSharedPtr<FCrowdyDeleteMark>>> MarkListView;
	TSharedPtr<SVerticalBox> FindingsBox;
	TSharedPtr<STextBlock> MarkedSummaryText;
	TSharedPtr<STextBlock> NoticeText;
	TSharedPtr<STextBlock> HeadlineText;
	TSharedPtr<STextBlock> AppLineText;
	TSharedPtr<STextBlock> CountsText;
	TSharedPtr<STextBlock> BlockedReasonText;
	TSharedPtr<STextBlock> ProgressText;
	TSharedPtr<STextBlock> AcknowledgeLabelText;
	TSharedPtr<SCheckBox> AcknowledgeCheckBox;
	TSharedPtr<SButton> ReviewButton;

	// The commit control's own label, set when the sheet is rebuilt. Held rather than bound because a bound text
	// attribute builds its string on every painted frame, and this one moves only when the plan does.
	TSharedPtr<STextBlock> CommitLabelText;

	// The commit control. Built non-focusable, and nothing in this panel ever moves keyboard focus to it, so
	// Enter pressed anywhere on the page cannot fire it.
	TSharedPtr<SButton> CommitButton;
};
