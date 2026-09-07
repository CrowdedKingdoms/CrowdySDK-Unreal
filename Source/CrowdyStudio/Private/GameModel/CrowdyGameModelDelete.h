// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyModelLedger.h" // ECrowdyModelRowKind, ECrowdyModelProvenance, FCrowdyModelRow, FCrowdyModelSummary
#include "Model/CrowdyStudioTypes.h"     // FStudioContainerType, FStudioPropertyDef, FStudioFunction, FStudioAutomation
#include "Templates/SharedPointer.h"

class FJsonObject;
struct FCrowdyModelSnapshot;

/**
 * Everything the Models browser needs to decide what a delete will do before it does it: what a marked set
 * costs, what order it has to run in, what the server will refuse, what the sheet says, what the button says,
 * and what is left over when a commit stops partway.
 *
 * PURE: no Slate, no HTTP, no UObject, no world. The caller gathers the evidence and issues the operations;
 * nothing here reads a server, a widget or an asset. That is what lets the one part of this page with no undo
 * be exercised without any of them.
 *
 * THE PAGE INVARIANT this layer exists to hold: the primary view never offers an action that will silently
 * undo itself. An entity the project declares can be deleted from the server and the next sync puts it back,
 * so the Models browser offers no delete on such a row at all; the honest escape hatch with the consequence
 * written on it lives in the Advanced tab. CanDeleteFromPrimaryView is the single place that rule is decided,
 * so a second surface cannot quietly disagree with it.
 *
 * VOCABULARY, binding on this surface: a container type is a MODEL, a property definition is an ATTRIBUTE, a
 * live container is a LIVE MODEL. The words container, invoke, edge, traverse and digest never reach a
 * heading, a button or a confirm here. The fixed nouns and severity words the text below composes come from
 * CrowdyModelVocabulary, which is the one place a value becomes designer-facing text; this file composes them
 * into sentences and never spells one of them out a second time.
 *
 * CASE. A model name, an attribute key, a function name, an automation name and a live model's id are all
 * server keys, and FString comparison folds case by default, as do TSet<FString> and TMap<FString>. Every
 * comparison here is case-sensitive and every keyed collection here is a TArray searched with an explicit
 * compare, exactly as CrowdyModelSnapshotKeys::KeysMatch already does. Two entities that differ only in case
 * are two entities, and folding them is how a delete lands on the wrong one.
 */

/** How many live models one pre-flight probe reads per marked model before it stops counting. */
inline constexpr int32 CrowdyDeleteLiveModelProbeLimit = 200;

/** How many references one finding lists before it says how many more it did not list. */
inline constexpr int32 CrowdyDeleteMaxDisclosureEntries = 20;

/**
 * What one delete acts on. Separate from ECrowdyModelRowKind because that one describes a row on screen while
 * this one selects a server mutation, and the two are free to diverge: a model is a row in the left-hand list
 * rather than a row in a table, and a future row kind need not be deletable at all. DeleteKindForRowKind is
 * the single mapping between them.
 */
enum class ECrowdyDeleteKind : uint8
{
	Model,
	Attribute,
	Function,
	Automation,
	LiveInstance
};

/**
 * How bad a finding is. Ordered so the worst of a set is its maximum, which is what the ladder is scaled to.
 *
 * Info is a documented cascade that is the intended consequence of what was asked for: deleting a model
 * removes its attributes. Caution means the delete will succeed and break something: deleting an attribute
 * orphans the values already stored on every live model and every expression that names the key, and nothing
 * server-side refuses or repairs that, so the pre-flight here is the only guard that exists anywhere.
 * Blocker means the server will refuse, predicted exactly from evidence in hand rather than guessed at.
 */
enum class ECrowdyDeleteSeverity : uint8
{
	Info = 0,
	Caution = 1,
	Blocker = 2
};

/**
 * What a finding is about. One entry per distinct consequence, never one per reference: an attribute read by
 * fourteen functions is one finding whose disclosure lists the fourteen, because fourteen rows saying the same
 * sentence is a wall the reader skips rather than an explanation they read.
 */
enum class ECrowdyDeleteFindingKind : uint8
{
	// BLOCKERS. The server refuses these, and the refusal is predictable from what the pre-flight read.

	// Deleting a model is refused while live models of it exist. Cleared by marking those live models in the same
	// plan, which the ordering below then runs before the model. There is deliberately no one-click "mark every
	// live model of this", because a control that silently adds forty-seven entries to a delete is not a control
	// anyone can check before pressing.
	ModelHasLiveModels,

	// The pre-flight could not count one marked model's live models, so nothing can be said about whether the
	// server will refuse. This is the answer for a missing scope, and it exists because the alternative is
	// worse: an absent count read as zero turns a refusal nobody was warned about into a green light. It
	// clears with a refresh, so it strands nobody.
	ModelLiveCountUnknown,

	// Deleting a model is refused while functions are still bound to it. Cleared by marking those functions in
	// the same plan, which the ordering below then runs first.
	ModelHasBoundFunctions,

	// CAUTIONS. These succeed, and something the reader cares about stops working.

	// Deleting an attribute leaves the values already stored for it on every live model of that model. The
	// server neither refuses this nor cleans up after it.
	AttributeOrphansStoredValues,

	// Functions read or write this attribute by name. Their expressions keep naming a key that no longer
	// exists. Every expression a function carries counts, not only its writes: an authority gate, a timer's delay,
	// a notification argument and the returned value all name attribute keys and all stop resolving.
	AttributeReadByFunctions,

	// Automations name this attribute, in the rule that picks their targets or in the event they wait for. They
	// survive, and the one that waited for a change to the key can never fire again.
	AttributeUsedByAutomations,

	// Deleting a model takes its attributes with it, and something outside the model still names one of them. The
	// consequence is the same as deleting the attribute directly, so it carries the same severity rather than
	// being softened into the cascade line just because it arrived through the model.
	ModelCascadeOrphansAttributeReaders,

	// gameModelDeleteFunction takes a bare name and this app has that name on more than one model, so nothing
	// here can promise the delete stays inside the model the reader is looking at. Whether the mutation is
	// app-wide or scoped by model is not settled, and a confirm that promises scoping is a promise the API may
	// not keep, so the finding names every model carrying the name and the sheet claims nothing.
	FunctionNameNotUniqueInApp,

	// An automation runs this function, or an event trigger names it. Whether the server refuses a function delete
	// while an automation still points at it is not settled, so this says only what is certain either way: the
	// automation is not repaired, and what it runs stops resolving.
	FunctionRunByAutomations,

	// An automation targets this model. It survives with a target that no longer exists.
	ModelTargetedByAutomations,

	// This live model was created under a binding key, so the runtime recreates it the next time that key is
	// ensured. Deleting it is not final, and its stored values do not come back with it.
	LiveModelIsRecreatedByItsBinding,

	// The project declares this entity, so the next sync recreates it. The primary view never offers such a
	// delete, and this finding is what catches a mark that reached the plan by some other route.
	RecreatedByTheNextSync,

	// INFO. Documented cascades, which is what the reader asked for.

	// Deleting a model removes its attributes with it.
	ModelCascadesItsAttributes,

	// Deleting a model also takes the revision attribute and touch function the SDK provisions on every model.
	// Neither is ever shown as a row, so the plan adds their deletes itself; without that the server refuses
	// the model delete over a function the reader was never shown and cannot mark.
	ModelCascadesItsPlumbing,

	// Deleting an automation removes its event triggers with it.
	AutomationCascadesItsTriggers,

	// Deleting a live model removes its stored values and the edges connected to it.
	LiveModelCascadesItsValuesAndEdges
};

/** How much consent one plan needs before it can run. Scaled to the worst finding, never to the op count. */
enum class ECrowdyDeleteLadder : uint8
{
	// Nothing marked, or nothing marked that has anything on the server to delete. No sheet and no button.
	Empty,

	// The worst finding is informational. One line, one button, and nothing to tick: a plan that only says
	// "this is what you asked for" must not cost more to confirm than it is worth.
	Confirm,

	// At least one caution. The button stays disabled until the acknowledgement is ticked, because a reader
	// who has not read the caution has not consented to it.
	Acknowledge,

	// At least one blocker. The button never arms. Clearing a blocker is a change to the marked set or a
	// refresh, never a stronger confirmation.
	Blocked
};

/** What one server delete reply said. */
enum class ECrowdyDeleteReply : uint8
{
	// The entity existed and is gone.
	Removed,

	// The entity was not there. An idempotent no-op, and a SUCCESS: a walk that reads this as a failure stops
	// on the one case that needed no work at all, which is what a re-click after a partial commit produces on
	// every op it already finished.
	AlreadyGone,

	// The reply carried no boolean under the field the operation declares. Not a success: the transport has
	// already sorted refusals into the failure path, so a clean envelope of an unexpected shape says nothing
	// about whether the entity survived, and continuing would report a delete that may not have happened.
	Unrecognized
};

/** Whether the pre-flight's count of one model's live models is a fact, a floor, or absent. */
enum class ECrowdyLiveCountState : uint8
{
	// Never read, or the read failed. NOT zero. Nothing can be claimed about this model, and a caller that
	// treats this as "none" converts a refusal into a green light.
	Unknown,

	// Read to the end. The count is the total.
	Exact,

	// The probe stopped at its limit. The count is a floor, and the phrasing that names it has to say so.
	AtLeast
};

/**
 * One entity marked for deletion, carrying the scope the server resolves its name in rather than a bare name.
 *
 * OwningType is the model that scopes this entity: the model an attribute or a function belongs to, and the
 * model a live model is an instance of. It is EMPTY for a model itself (whose own name is in Name) and for an
 * automation, whose name the server keeps unique app-wide. That is the same convention as
 * FCrowdySchemaSync::ScopedNameKey, so a mark and the diff can never disagree about which entity is which.
 * OwningModelName folds the model case for a caller that wants "which model does this belong to" in one call.
 *
 * Build one with the factories below rather than by hand. A mark assembled field by field is one typo away
 * from naming a different entity, and the field it would be wrong in is the one nothing downstream can check.
 */
struct FCrowdyDeleteMark
{
	ECrowdyDeleteKind Kind = ECrowdyDeleteKind::Attribute;

	FString OwningType;

	// Verbatim from the server: the model name, the attribute key, the function name, the automation name, or
	// the live model's id.
	FString Name;

	// The designer-facing label the sheet and the marked list show. Never identity: two entities with the same
	// display name are still two entities, and nothing compares this.
	FString Display;

	// The model this belongs to: OwningType, or Name when this IS a model.
	FString OwningModelName() const;

	// Kind plus the scoped name, as one string. The kind is part of it because an attribute and a function on
	// one model can share a name and are still two entities.
	FString IdentityKey() const;

	// Case-SENSITIVE on every field that is identity. Declared so TArray::Contains and TArray::Remove are
	// correct on a mark array; the default FString comparison would fold two distinct server keys into one.
	bool operator==(const FCrowdyDeleteMark& Other) const;
	bool operator!=(const FCrowdyDeleteMark& Other) const { return !(*this == Other); }
};

/** One model's attributes as the pre-flight had them. */
struct FCrowdyDeleteModelAttributes
{
	FString TypeName;
	TArray<TSharedPtr<FStudioPropertyDef>> Attributes;
};

/**
 * How many live models one model has, as the pre-flight's own scoped read found them.
 *
 * The read that produces this must be side-effect free. The shared container list the Live tab renders is
 * filtered by whatever the reader last typed into its boxes, so counting from it would report zero for a model
 * with four hundred instances and turn a blocker into a green light. The count has its own destination for
 * that reason and for no other.
 */
struct FCrowdyDeleteLiveCount
{
	FString TypeName;
	ECrowdyLiveCountState State = ECrowdyLiveCountState::Unknown;
	int32 Count = 0;

	// A few ids for the finding's disclosure, bounded by CrowdyDeleteMaxDisclosureEntries. A reader who wants
	// all of them wants the Live tab, not a confirm sheet.
	TArray<FString> SampleIds;
};

/**
 * Everything a plan is judged against, gathered by the caller because nothing here does I/O.
 *
 * Held by value. The arrays are arrays of shared pointers, so copying one costs a pointer per entry and, more
 * to the point, keeps the entities alive for the life of the review: the controller replaces these arrays
 * wholesale when a read lands, and a review holding a pointer into the old array would be reading rows that
 * were replaced under it while its sheet still named them.
 *
 * READ FLAGS. An empty array means "this app has none" only when its flag says the read happened. Otherwise it
 * means nobody asked, and the two must never be folded: an unread function list read as empty clears the
 * bound-functions blocker for every model in the app at once.
 */
struct FCrowdyDeleteEvidence
{
	int64 AppId = 0;

	// The last finished plan for this app, or null when none has run. Provenance is what decides whether an
	// entity is declared in code, and without a plan there is no answer, which is a reason to offer no delete
	// rather than a reason to guess.
	TSharedPtr<const FCrowdyModelSnapshot> Snapshot;

	TArray<TSharedPtr<FStudioContainerType>> Types;

	// Every function of the app, unfiltered. It must be the unfiltered list: the mirror a view narrows to one
	// model would report that no other model has a function bound to it, which is the bound-functions blocker
	// answering from a list that was never asked the question.
	TArray<TSharedPtr<FStudioFunction>> Functions;

	TArray<TSharedPtr<FStudioAutomation>> Automations;
	TArray<TSharedPtr<FStudioAutomationTrigger>> AutomationTriggers;

	// The attributes of whichever models have been read, one entry per model. A model with no entry has not
	// been read, which is different from a model with an empty entry, and the cascade line phrases itself
	// without a count rather than claiming zero.
	TArray<FCrowdyDeleteModelAttributes> AttributesByModel;

	// One entry per model the pre-flight probed. A marked model with no entry is Unknown, which is a blocker.
	TArray<FCrowdyDeleteLiveCount> LiveCounts;

	bool bTypesRead = false;
	bool bFunctionsRead = false;
	bool bAutomationsRead = false;

	// The attributes held for one model, or null when that model has never been read.
	const TArray<TSharedPtr<FStudioPropertyDef>>* FindAttributes(const FString& TypeName) const;

	// One model's live count. Returns Unknown and leaves OutCount at zero when the model was never probed;
	// there is deliberately no overload that answers with a bare int, because a bare int cannot say "unknown"
	// and every caller of one would spell it zero.
	ECrowdyLiveCountState FindLiveCount(const FString& TypeName, int32& OutCount) const;
	const FCrowdyDeleteLiveCount* FindLiveCountEntry(const FString& TypeName) const;
};

/**
 * One consequence of one mark, with every reference to it in one place.
 *
 * References are already phrased for display and bounded at CrowdyDeleteMaxDisclosureEntries; ReferenceCount
 * is the real total, so a disclosure that had to stop can still say how many there are.
 */
struct FCrowdyDeleteFinding
{
	ECrowdyDeleteFindingKind Kind = ECrowdyDeleteFindingKind::ModelCascadesItsAttributes;
	ECrowdyDeleteSeverity Severity = ECrowdyDeleteSeverity::Info;

	// The mark this is about. A finding always belongs to exactly one mark, so a reader can go from a line on
	// the sheet back to the row that produced it.
	FCrowdyDeleteMark Subject;

	// One sentence naming the subject and the count. Complete on its own: a reader who never opens the
	// disclosure still knows what will happen.
	FString Headline;

	// What to do about it, or empty when there is nothing to do. Empty is the right answer for an
	// informational cascade, whose whole content is that it is intended.
	FString Remedy;

	// The entities this finding is about, one line each.
	TArray<FString> References;

	// How many there are in total, which exceeds References.Num() when the list was cut short.
	int32 ReferenceCount = 0;
	bool bReferencesTruncated = false;
};

/**
 * One server mutation the commit will issue, fully resolved.
 *
 * StringArgs carries the mutation's arguments in the order the operation declares them, so the walk that
 * issues them writes one generic loop rather than a switch that has to be kept in step with this file. appId
 * is deliberately absent: every server id is a BigInt and has to be emitted as a JSON string, which is the one
 * argument the walk cannot set generically, and leaving it out here means it can never be set the wrong way.
 */
struct FCrowdyDeleteOp
{
	ECrowdyDeleteKind Kind = ECrowdyDeleteKind::Attribute;

	// The mark that produced this op. An implied op carries the mark of the model that implied it.
	FCrowdyDeleteMark Subject;

	// The generated operation name, e.g. GameModelDeleteFunction.
	FString OperationName;

	// The field in the reply's data object that carries the boolean, e.g. gameModelDeleteFunction.
	FString ResultField;

	TArray<TPair<FString, FString>> StringArgs;

	// One line for the progress and remainder text, e.g. "the function regen on Knight".
	FString Describe;

	// True for an op the plan added on its own rather than one the reader marked: the revision attribute and
	// touch function the SDK provisions on every model, which are never rows and which the server would
	// otherwise refuse the model delete over. Counted separately on the sheet so the numbers on screen match
	// the rows the reader ticked.
	bool bImplied = false;

	// True when this app carries this function name on more than one model. The sheet's line for such an op
	// must not promise the delete stays inside one model, because the mutation takes a bare name.
	bool bNameNotUniqueInApp = false;
};

/** The sheet's text, built once when the plan is built rather than in a Slate attribute read every frame. */
struct FCrowdyDeleteSheet
{
	// "Delete 4 models, 12 attributes and 2 functions?" Names the kinds and their counts and nothing else.
	FString Headline;

	// The app this writes to, named rather than implied, so a reader who switched apps sees it here.
	FString AppLine;

	// One line per kind present, in commit order.
	TArray<FString> CountLines;

	// The entries the plan added itself, phrased as one line, or empty when it added none.
	FString ImpliedLine;

	// The marks whose work another mark already does, phrased as one line, or empty when there are none.
	FString SubsumedLine;

	// What the button says. It always names the count it is about, so the action and the number it acts on
	// cannot be read apart.
	FString ActionLabel;

	// The acknowledgement's exact words, empty when the ladder needs none.
	FString AcknowledgeLabel;

	// Why the button is disabled, empty when it is not. This is the gate text a reader acts on, so it names
	// what to change rather than merely restating that something is wrong.
	FString BlockedReason;
};

/**
 * A marked set, judged. Everything the review panel renders and everything the commit runs.
 *
 * Built by BuildPlan and never mutated afterwards: the consent token below fingerprints exactly this, so a
 * plan edited after it was shown is a plan the reader never consented to.
 */
struct FCrowdyDeletePlan
{
	int64 AppId = 0;

	// The marks as given, in the caller's order, so the marked list on screen keeps the order it was built in.
	TArray<FCrowdyDeleteMark> Marks;

	// The marks whose work another mark already does, so they contribute no op of their own. Today that is one
	// relationship: an attribute of a model that is itself marked, since the server removes a model's
	// attributes with it. Kept rather than dropped so the sheet can account for every row the reader ticked.
	TArray<FCrowdyDeleteMark> SubsumedMarks;

	// Worst first, then by kind, then by subject, so the same marked set always produces the same order and a
	// reader's eye lands on what stops the commit before it lands on what merely accompanies it.
	TArray<FCrowdyDeleteFinding> Findings;

	// In commit order: automations, live models, attributes, functions, models. Within a kind, ordered by
	// owning model then name so the same marked set always produces the same list whatever order it was
	// ticked in, which is what makes the consent token below stable.
	TArray<FCrowdyDeleteOp> Ops;

	ECrowdyDeleteLadder Ladder = ECrowdyDeleteLadder::Empty;

	FCrowdyDeleteSheet Sheet;

	// A fingerprint of the app, the ops and the ladder. NOT something anybody types: it is how the panel
	// proves at click time that the plan it is about to run is the plan the sheet on screen described. A read
	// landing between the sheet being drawn and the button being pressed changes the ops and therefore this,
	// and the commit is refused rather than running against consent given for a different set.
	FString ConsentToken;

	// False whenever a blocker is present, whenever there is nothing to do, and whenever the evidence was not
	// good enough to judge. The commit path checks this too, so the rule is enforced away from the widget that
	// happens to render it.
	bool bCommittable = false;

	// How many ops of one kind will run. Counts ops rather than marks, so a subsumed attribute is not counted
	// and an implied plumbing op is counted once.
	int32 CountOps(ECrowdyDeleteKind Kind) const;

	// How many ops the plan added on its own.
	int32 CountImpliedOps() const;
};

/** What one commit walk did. */
struct FCrowdyDeleteOutcome
{
	int64 AppId = 0;

	// How many ops the walk set out to run.
	int32 Total = 0;

	// How many finished, counting the ones that were already gone: an entity that was not there is an entity
	// that does not need deleting, and a walk that stops on one stops on the case that needed no work.
	int32 Completed = 0;

	// How many of those replied that there was nothing there. Reported honestly rather than folded into
	// Completed, because "16 deleted, 2 were already gone" and "18 deleted" are different facts.
	int32 AlreadyGone = 0;

	bool bStopped = false;

	// Where it stopped, or INDEX_NONE when it finished. The op AT this index did not complete, so it is the
	// first entry of the remainder.
	int32 StoppedAtIndex = INDEX_NONE;

	// True when the stop was this editor tearing down its own client rather than the server answering. The two
	// need different wording, because one sends a reader hunting for a blocker that was never there.
	bool bStoppedByCancel = false;

	// The op that did not complete, phrased, or empty when the walk finished.
	FString StoppedOnDescription;
};

/** Whether one row may be deleted from the primary view, and what to say and offer when it may not. */
struct FCrowdyDeleteGate
{
	bool bAllowed = false;

	// One sentence saying why not, written for a reader who has only the row in front of them. Empty when
	// allowed.
	FString Reason;

	// The class or asset that declares it, as a path the editor can open, so the row can offer somewhere to go
	// instead of a disabled button with nothing behind it. Empty when there is nothing to open.
	FString CodePath;
};

namespace CrowdyGameModelDelete
{
	// The order every commit runs in, and the only place it is stated. Automations first, because an automation
	// names the function it runs and removing the automation first cannot leave anything pointing at a function
	// that has gone; then live models, which a model delete IS refused over; then attributes; then functions,
	// which a model delete is likewise refused over; then models. This is the existing schema prune's order with
	// live models inserted, and it is a fixed list rather than a rule so a test can compare against it byte for
	// byte.
	const TArray<ECrowdyDeleteKind>& CommitOrder();

	// The single mapping between a row on screen and the mutation that deletes it.
	ECrowdyDeleteKind DeleteKindForRowKind(ECrowdyModelRowKind RowKind);

	// The mark factories. Every one of them fills the scope its kind requires and leaves empty the one its
	// kind does not have, so a mark can never claim a scope the server does not resolve names in.
	//
	// Prefer the two that take a ledger row or summary whole: a destructive control's target has to come from
	// the widget that shows it, read at the moment of the click, and handing over the row itself is what makes
	// re-typing its fields somewhere else impossible.
	FCrowdyDeleteMark MarkFromRow(const FCrowdyModelRow& Row);
	FCrowdyDeleteMark MarkFromModel(const FCrowdyModelSummary& Model);

	FCrowdyDeleteMark MarkModel(const FString& TypeName, const FString& Display);
	FCrowdyDeleteMark MarkAttribute(const FString& OwningType, const FString& Key, const FString& Display);
	FCrowdyDeleteMark MarkFunction(const FString& OwningType, const FString& Name, const FString& Display);
	FCrowdyDeleteMark MarkAutomation(const FString& Name, const FString& Display);
	FCrowdyDeleteMark MarkLiveModel(const FString& OwningType, const FString& ContainerId, const FString& Display);

	// Whether a marked set already holds this exact entity. Case-sensitive on every identity field.
	bool ContainsMark(const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteMark& Mark);

	// One mark as the sentence the marked list shows. It never promises a scoping the mutation may not honour: a
	// function whose name this app carries on more than one model is named without a model and says so, because
	// the marked list is the only place the reader is told what they picked before they press Review.
	FString DescribeMark(const FCrowdyDeleteMark& Mark, const FCrowdyDeleteEvidence& Evidence);

	// True when another mark in the set already does this mark's work, so it contributes no op of its own.
	// One relationship qualifies today: deleting a model removes its attributes. A function on a marked model
	// is deliberately NOT subsumed, because a model delete is refused while functions are bound to it, so the
	// function's own op is what clears the blocker rather than something the model absorbs.
	bool IsSubsumed(const FCrowdyDeleteMark& Mark, const TArray<FCrowdyDeleteMark>& Marks);

	// THE GATE. Whether the Models browser may offer a delete on this row at all.
	//
	// An entity the project declares can be deleted and the next sync recreates it, which is work undoing
	// itself with a success message in between, so every provenance that means "code declares this"
	// (code-synced, code-not-pushed, code-drifted, kit-owned) is refused here with the declaring class or
	// asset to open instead.
	//
	// Unknown provenance is refused too, and that is the point rather than an oversight: unknown means no plan
	// has run, so whether the project declares this entity is a question nobody has asked. Offering the delete
	// would be answering it by assumption in the direction that loses work. The remedy names the one press
	// that settles it.
	//
	// A live model is the exception and is always allowed: it is runtime state, no class declares one, and a
	// sync neither creates nor recreates it, so the trap this gate exists for cannot apply.
	//
	// Both overloads take the widget's own row or summary rather than a provenance value, so a caller cannot
	// hand over a classification it worked out somewhere else.
	FCrowdyDeleteGate CanDeleteFromPrimaryView(const FCrowdyModelRow& Row);
	FCrowdyDeleteGate CanDeleteFromPrimaryView(const FCrowdyModelSummary& Model);

	// Whether the evidence is complete enough to judge a plan at all, and what is missing when it is not.
	// A review that opened on half-read evidence would render a clean sheet built from lists nobody read, so
	// this is checked before a plan is built rather than after it disagrees with the server.
	bool IsEvidenceSufficient(const FCrowdyDeleteEvidence& Evidence, FString& OutReason);

	// THE PLAN. Everything above, assembled. Pure and total: any marked set and any evidence produce a plan,
	// and a plan that cannot be committed says so in its ladder and its blocked reason rather than by being
	// absent.
	FCrowdyDeletePlan BuildPlan(const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteEvidence& Evidence);

	// The parts BuildPlan composes, exposed because each is a rule worth exercising on its own and because a
	// plan that is wrong is wrong in exactly one of them.
	TArray<FCrowdyDeleteFinding> BuildFindings(
		const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteEvidence& Evidence);
	TArray<FCrowdyDeleteOp> BuildOps(const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteEvidence& Evidence);
	FCrowdyDeleteSheet BuildSheet(
		int64 AppId, const TArray<FCrowdyDeleteOp>& Ops, const TArray<FCrowdyDeleteMark>& SubsumedMarks,
		const TArray<FCrowdyDeleteFinding>& Findings, ECrowdyDeleteLadder Ladder);

	// The worst of a set, or Info for an empty one.
	ECrowdyDeleteSeverity WorstSeverity(const TArray<FCrowdyDeleteFinding>& Findings);

	// The ladder for a set of findings over a given number of ops. Zero ops is Empty whatever the findings
	// say, since there is nothing to consent to.
	ECrowdyDeleteLadder LadderFor(const TArray<FCrowdyDeleteFinding>& Findings, int32 OpCount);

	// The fingerprint the panel compares at click time. Stable for the same app, the same ops in the same
	// order, and the same ladder; different for anything else, including one op's arguments changing while its
	// count stays the same.
	FString MakeConsentToken(int64 AppId, const TArray<FCrowdyDeleteOp>& Ops, ECrowdyDeleteLadder Ladder);

	// THE REPLY. What one delete mutation's envelope said, read from the { "data": { <field>: bool } } shape
	// every one of them returns.
	//
	// This has its own name and its own tests because it is the likeliest place on this surface to get one
	// character wrong: a false boolean means the entity was not there, which is an idempotent no-op and reads
	// as success. Treating it as a failure stops a walk on the op that needed no work, and a re-click after a
	// partial commit produces exactly that on every op the first attempt already finished, so the second click
	// could never finish the job.
	ECrowdyDeleteReply ReadDeleteReply(const TSharedPtr<FJsonObject>& Envelope, const FString& ResultField);

	// Whether a reply of this shape lets the walk continue. True for Removed and AlreadyGone, false for
	// Unrecognized.
	bool IsDeleteReplySuccess(ECrowdyDeleteReply Reply);

	// THE REMAINDER. The ops a stopped walk did not run, in the same order, starting with the one that
	// failed: it did not complete, so a second click has to run it again. An index of INDEX_NONE, or one past
	// the end, yields nothing.
	TArray<FCrowdyDeleteOp> Remainder(const TArray<FCrowdyDeleteOp>& Ops, int32 StoppedAtIndex);

	// Whether a remainder still describes work the plan on screen would actually do. A remainder outlives the plan
	// that produced it: nothing clears it when the marked set changes, so a banner reading "press Delete again to
	// finish the remaining 3" can end up standing over a button that would run something else entirely. True only
	// when every remaining operation is still in the plan, which is the condition under which that sentence is true.
	bool RemainderAppliesTo(const TArray<FCrowdyDeleteOp>& Remainder, const TArray<FCrowdyDeleteOp>& PlanOps);

	// The durable line a stopped walk leaves: how far it got, what it stopped on, and that a second press
	// finishes the rest. Deletes already made are gone and re-running them is a no-op, so this says finish
	// rather than start over.
	FString StopText(const FCrowdyDeleteOutcome& Outcome);

	// The line a finished walk leaves, naming what was deleted and what was already gone.
	FString CompletionText(const FCrowdyDeleteOutcome& Outcome);

	// The line a live-model purge leaves. It never says "N of M": the server sends no count of live models, so a
	// purge learns how many there were by draining them and any total it named up front would be invented.
	FString PurgeText(const FCrowdyDeleteOutcome& Outcome);

	// BULK. Every server-only entity in the app as marks, which is what the old Remove Server-Only control
	// becomes once it moves into this review: a way to mark a lot at once, in front of the same sheet and the
	// same ladder as one row, rather than a button beside a write control.
	//
	// Derived from the ledger's own classification rather than from a plan's server-only lists, because those
	// carry function names with no scope and nothing downstream can then say which model a name belonged to.
	TArray<FCrowdyDeleteMark> MarkEverythingServerOnly(const FCrowdyDeleteEvidence& Evidence);

	// Whether that bulk mark may be offered at all, and why not when it may not. It is refused with no plan
	// captured, since server-only is a classification and there is none; and refused on any app a Game Kit has
	// been deployed to, because only kit FUNCTIONS carry prune protection today and a bulk mark would offer
	// the kit's own models and attributes for deletion.
	bool CanMarkEverythingServerOnly(const FCrowdyDeleteEvidence& Evidence, FString& OutReason);

	// BULK, UNCONDITIONAL. Every entity the server holds for this app, whatever declares it, so code-backed and
	// kit-owned ones too. Each of those raises its own caution, which holds the commit button until acknowledged.
	TArray<FCrowdyDeleteMark> MarkEverythingOnServer(const FCrowdyDeleteEvidence& Evidence);

	// Whether that mark may be offered. Needs a plan so the findings can name what comes back, and the lists read.
	// A deployed Game Kit does not refuse it, unlike the server-only mark.
	bool CanMarkEverythingOnServer(const FCrowdyDeleteEvidence& Evidence, FString& OutReason);

	// THE REFERENCE SCANS. Each is one rule, named so it can be exercised without a plan around it, and each
	// compares server keys case-sensitively.

	// Whether Text names Identifier as a whole word. A dot, a bracket and whitespace all end an identifier, so
	// "self.hp" names "hp" while "hp_max" does not: a scan for an attribute key must not report every expression
	// that happens to mention a longer key starting with the same letters. Case-sensitive, like every server key.
	//
	// Declared here because more than one layer needs whole-identifier matching and the unity build merges this
	// module's .cpp files into one translation unit, so a second copy in another .cpp would redefine this one.
	bool NamesIdentifier(const FString& Text, const FString& Identifier);

	// Whether one function names an attribute key ANYWHERE it can. A function is not only its writes: its
	// authority gate, the value it answers with, the arguments of every notification it emits and the delay,
	// dedupe key and bound parameters of every timer it arms are all expressions over model state, and every one
	// of them keeps naming a key that has gone. Matching is on whole identifiers, so deleting "hp" does not report
	// a function that only ever mentions "hp_max".
	bool FunctionReferencesAttribute(const FStudioFunction& Function, const FString& OwningType, const FString& Key);

	// Whether one automation names an attribute key, in the rule that picks its targets or in the static
	// parameters it passes. Both are JSON the server reads as model expressions.
	bool AutomationReferencesAttribute(const FStudioAutomation& Automation, const FString& Key);

	// Whether one event trigger waits on an attribute key. A trigger names the model it watches, so this narrows
	// by it; a trigger whose model this app never reported is matched anyway, because a missing scope is a scope
	// nobody determined and reading it as "some other model" is how a real dependant goes unreported.
	bool TriggerReferencesAttribute(
		const FStudioAutomationTrigger& Trigger, const FString& OwningType, const FString& Key);

	// Whether one automation runs a function by name.
	bool AutomationReferencesFunction(const FStudioAutomation& Automation, const FString& FunctionName);

	// Whether one event trigger names a function, either as the function it runs or as the invocation it
	// reacts to.
	bool TriggerReferencesFunction(const FStudioAutomationTrigger& Trigger, const FString& FunctionName);

	// Whether one automation targets a model, by target type or by an event trigger's container type.
	bool AutomationTargetsModel(const FStudioAutomation& Automation, const FString& TypeName);

	// Every model in this app carrying a function of this name, in name order. Models only: a record whose model
	// the read never carried has no name to give, so it cannot appear here.
	TArray<FString> ModelsCarryingFunctionName(const FCrowdyDeleteEvidence& Evidence, const FString& FunctionName);

	// Every SCOPE this app carries a function of this name in, phrased for a reader. That is the models above plus
	// one line for a record whose model was never determined, which is its own scope and is exactly the one that
	// makes a bare-name delete ambiguous. The caution is raised from the size of this and disclosed from its
	// contents, so the list a reader opens can never omit the entity that raised it.
	TArray<FString> ScopesCarryingFunctionName(
		const TArray<TSharedPtr<FStudioFunction>>& Functions, const FString& FunctionName);
	TArray<FString> ScopesCarryingFunctionName(const FCrowdyDeleteEvidence& Evidence, const FString& FunctionName);

	// The functions bound to a model that the plan does not already delete: what the model's delete will be
	// refused over. Reserved plumbing is excluded, since the plan deletes that itself.
	TArray<FString> FunctionsBlockingModelDelete(
		const FString& TypeName, const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteEvidence& Evidence);

	// How many live models of one model the plan leaves standing: the pre-flight's count less the live models
	// marked in the same plan, which the commit order runs before the model. Same rule as the functions above, and
	// for the same reason: an entity this plan deletes first cannot be what the server refuses over. Never
	// negative, and a count that was only a floor stays a floor, so marking a handful cannot talk a capped probe
	// down to zero.
	int32 LiveModelsBlockingModelDelete(
		const FString& TypeName, const TArray<FCrowdyDeleteMark>& Marks, const FCrowdyDeleteEvidence& Evidence);

	// The SDK plumbing functions the server actually holds for a model, which is what the model's implied ops
	// delete. Read from the evidence rather than composed from a naming rule, so the plan only ever issues a
	// delete for something that is really there.
	TArray<FString> PlumbingFunctionsFor(const FString& TypeName, const FCrowdyDeleteEvidence& Evidence);
}
