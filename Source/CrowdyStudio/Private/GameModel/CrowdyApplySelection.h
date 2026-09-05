// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdySchemaSync.h" // FCrowdySchemaTypeUpsert, FCrowdySchemaPropUpsert, ... , FCrowdyGameModelFunctionInput

/**
 * Which pending upserts a selection actually sends, what had to be added to make that safe, in what order, and
 * whether it can be sent at all.
 *
 * PURE: no Slate, no HTTP, no UObject, no world. The caller gathers the pending plan and issues the operations;
 * nothing here reads a server, a widget or an asset. That is what lets the one part of this page that can write
 * broken schema to a live server be exercised without any of them.
 *
 * CASE. A model name, an attribute key, a function name and an automation name are all server keys, and FString
 * comparison folds case by default, as do TSet<FString> and TMap<FString>. Every comparison here is
 * case-sensitive and every keyed collection here is a TArray searched with an explicit compare. Two entities that
 * differ only in case are two entities, and folding them is how a selection sends the wrong one.
 *
 * SAFETY, in one sentence: the arrays this reads are a DIFF. An entity absent from them is already correct on the
 * server, which is the only reason sending a subset of them can ever be safe.
 */

/**
 * What one selectable unit acts on. Separate from ECrowdyModelRowKind because that describes a row on screen while
 * this selects a server upsert, and separate from ECrowdyDeleteKind because a trigger has no delete of its own.
 * CrowdyApplySelection::ApplyOrder is the single statement of the order these run in.
 */
enum class ECrowdyApplyKind : uint8
{
	Type,
	Attribute,
	Function,
	Automation,
	Trigger
};

/**
 * Whether a unit's scope is a determined answer. A scope that is empty on a kind that requires one is NOT
 * "app-wide": a record whose owning model was never resolved and a record that genuinely has none are
 * indistinguishable at this point, and only one of them is safe to act on.
 */
enum class ECrowdyApplyScope : uint8
{
	// The kind carries no scope at all: a model, an automation, a trigger.
	NotScoped,

	// The kind carries a scope and it is present.
	Determined,

	// The kind carries a scope and it is empty. Its own answer, never a fall-through.
	Undetermined
};

/** Why a closure could not be completed. Every value has its own sentence; there is no generic failure. */
enum class ECrowdyApplyRefusal : uint8
{
	None,

	// Nothing is selected, or everything selected turned out to have no op.
	NothingSelected,

	// Something the selection needs is in the plan but cannot be identified, because its owning model was never
	// determined. Sending the selection without it writes a reference to something the server may not have;
	// sending it with it writes an upsert nobody can say is the right one.
	UnresolvablePrerequisite,

	// The selection names units this plan does not hold. The plan was recomputed under it.
	SelectionIsStale,

	// No plan is pending.
	NoPlan
};

/** One entity a plan would upsert, as the selection surface lists it. */
struct FCrowdyApplyUnit
{
	ECrowdyApplyKind Kind = ECrowdyApplyKind::Type;

	// The model that scopes this unit: the container type an attribute or a function belongs to, and the container
	// type a trigger filters on when it has one. EMPTY for a model (whose own name is in Name) and for an
	// automation, whose name the server keeps unique app-wide. Same convention as FCrowdySchemaSync::ScopedNameKey
	// and FCrowdyDeleteMark::OwningType, so a unit, a mark and the diff can never disagree about which entity is
	// which.
	FString OwningType;

	// Verbatim from the plan: the type name, the attribute key, the function name, the automation name, or for a
	// trigger the automation name it fires.
	FString Name;

	// A trigger's identity beyond its automation name: the event and every filter field, canonically joined. Empty
	// for every other kind. The field list mirrors the one the schema diff keys a trigger on (the automation name,
	// the event, and the function, model and property-key filters) and has to keep mirroring it. The write source
	// is NOT in it, because the diff treats that as a tunable it updates a trigger with rather than as a different
	// trigger; a unit finer than the diff's key silently drops a selection across a re-plan, and a coarser one
	// folds two distinct triggers into one.
	FString Discriminator;

	// The designer-facing label the list shows. Never identity: two entities with the same display name are still
	// two entities, and nothing compares this.
	FString Display;

	// Whether the plan CREATES this entity or merely updates one the server already has. Load-bearing: a
	// prerequisite that already exists on the server is not a prerequisite. See ClosePrerequisites.
	bool bIsNew = false;

	// SDK wiring the sync provisions itself (the revision attribute and the per-model touch function). Never listed
	// and never selectable; carried into the closed set with anything else on its model, because the pair is what
	// makes a model-collection change observable and applying one half breaks it.
	bool bReserved = false;

	// Where in the plan's own arrays this unit came from, so the op list can be rebuilt in plan order without
	// re-deriving it. Kind plus this index addresses exactly one pending upsert.
	int32 PlanIndex = INDEX_NONE;

	ECrowdyApplyScope Scope() const;
	bool HasDeterminedScope() const;

	// The model this belongs to: OwningType, or Name when this IS a model. Empty for an automation.
	FString OwningModelName() const;

	// Kind plus the scoped name plus the discriminator, as one string. Stable across a re-plan, which is what lets
	// a selection survive a rebuild of the pending arrays by identity rather than by index.
	FString IdentityKey() const;

	// Case-SENSITIVE on every identity field. Declared so TArray::Contains and TArray::Remove are correct; the
	// default FString comparison folds two distinct server keys into one.
	bool operator==(const FCrowdyApplyUnit& Other) const;
	bool operator!=(const FCrowdyApplyUnit& Other) const { return !(*this == Other); }
};

/** One unit the closure added on the user's behalf, with the reason. */
struct FCrowdyApplyAddition
{
	FCrowdyApplyUnit Unit;

	// The IdentityKey of the SELECTED unit that forced this one in. Never empty: a unit with no forcing unit is not
	// an addition.
	FString ForcedByKey;

	// One sentence naming both ends and the field that connects them, written for a reader who has only this line.
	FString Because;
};

/**
 * Everything a plan is built from, by pointer, because the controller owns these arrays and they are read once.
 * A null array reads as an empty one, so a partially filled input still produces an answer.
 */
struct FCrowdyApplyPlanInput
{
	int64 AppId = 0;
	const TArray<FCrowdySchemaTypeUpsert>* Types = nullptr;
	const TArray<FCrowdySchemaPropUpsert>* Props = nullptr;
	const TArray<FCrowdySchemaFunctionUpsert>* Functions = nullptr;
	const TArray<FCrowdySchemaAutomationUpsert>* Automations = nullptr;
	const TArray<FCrowdySchemaTriggerUpsert>* Triggers = nullptr;
};

/** The sheet's text, built once when the plan is built rather than in a Slate attribute read every frame. */
struct FCrowdyApplySheet
{
	// "Send 3 models, 11 attributes and 2 functions?" Names the kinds and their counts and nothing else.
	FString Headline;

	// The app this writes to, named rather than implied.
	FString AppLine;

	// One line per kind present, in apply order.
	TArray<FString> CountLines;

	// "2 more were added because something you picked needs them." Empty when the closure added nothing.
	FString AddedLine;

	// "plus the wiring the SDK keeps on 2 models." Empty when no reserved unit entered the set.
	FString ReservedLine;

	// "14 of 27 changes stay unsent." Empty when everything is selected.
	FString RemainderLine;

	// What the button says. It always names the count it is about.
	FString ActionLabel;

	// Why the button is disabled, empty when it is not. Names what to change, not merely that something is wrong.
	FString BlockedReason;
};

/** A selection, closed and judged. Everything the panel renders and everything the apply walk runs. */
struct FCrowdyApplyPlan
{
	int64 AppId = 0;

	// Every unit the plan holds, in plan order within each kind and kinds in ApplyOrder. The selection surface
	// renders this minus the reserved units, which CrowdyApplySelection::SelectableUnits drops.
	TArray<FCrowdyApplyUnit> AllUnits;

	// What the user ticked, by identity, in the order they were given, after any key this plan does not hold was
	// dropped.
	TArray<FString> SelectedKeys;

	// The closure: everything that will actually be sent, in apply order.
	TArray<FCrowdyApplyUnit> ClosedSet;

	// The subset of ClosedSet that nobody ticked, each naming what forced it.
	TArray<FCrowdyApplyAddition> Additions;

	// What stays behind, in apply order. Exactly what a second, different selection would still have to send, which
	// is why the reserved units it strands are in it: they stay pending on the server side too.
	TArray<FCrowdyApplyUnit> Remainder;

	ECrowdyApplyRefusal Refusal = ECrowdyApplyRefusal::None;

	// The units that could not be resolved, when Refusal is UnresolvablePrerequisite. Named so the panel can say
	// which model to fix rather than only that something is wrong.
	TArray<FCrowdyApplyUnit> Unresolvable;

	// The selected keys this plan no longer holds. A partial drop is not a refusal: the surviving selection is
	// closed and sent, and the caller says how many went.
	TArray<FString> DroppedKeys;

	FCrowdyApplySheet Sheet;

	// A fingerprint of the app, the closed set in order, and the refusal. NOT something anybody types: it is how
	// the panel proves at click time that the plan it is about to send is the plan the sheet described. A read
	// landing between the sheet being drawn and the button being pressed changes it and the apply is refused.
	FString ConsentToken;

	// False for every refusal and for an empty closed set. The controller checks this too, so the rule is enforced
	// away from the widget that renders it.
	bool bSendable = false;

	int32 CountUnits(ECrowdyApplyKind Kind) const; // over ClosedSet
	int32 CountAdditions() const;
	int32 CountReserved() const;
};

namespace CrowdyApplySelection
{
	// The order every apply runs in, and the only place it is stated. Types, then attributes (a new attribute needs
	// its type), then functions (a function names its type and the keys it writes), then automations (an automation
	// names its function), then triggers (a trigger names its automation). This is the existing apply's five loops
	// as a fixed list rather than a rule, so a test compares against it byte for byte.
	const TArray<ECrowdyApplyKind>& ApplyOrder();

	// Every pending upsert as a unit, in apply order, with bIsNew and bReserved filled from the plan. Reserved units
	// ARE produced: they participate in the closure like anything else and are merely never listed.
	TArray<FCrowdyApplyUnit> BuildUnits(const FCrowdyApplyPlanInput& Plan);

	// The units a selection surface may show. BuildUnits minus everything bReserved, which is the same exclusion
	// CrowdyModelLedger applies to rows, so the two surfaces cannot disagree about what exists.
	TArray<FCrowdyApplyUnit> SelectableUnits(const TArray<FCrowdyApplyUnit>& Units);

	// Whether one unit may be ticked at all, and why not when it may not. Refused for a unit whose kind requires a
	// scope and whose scope is undetermined: nothing can say which model it belongs to, so nothing can say what it
	// needs or what needs it.
	bool CanSelect(const FCrowdyApplyUnit& Unit, FString& OutReason);

	// THE CLOSURE. Every unit the selection needs, added transitively, each naming the selected unit that forced it.
	// Pure and total: any selection and any plan produce an answer, and one that cannot be made safe says so in its
	// refusal rather than by being absent.
	//
	// A prerequisite has exactly THREE answers, not two, and folding any pair of them is how this writes broken
	// schema to a live server:
	//   1. The plan holds it and it is a CREATE (bIsNew). It must be added.
	//   2. The plan holds it only as an UPDATE, or does not hold it at all. The server already has the entity, so it
	//      is not a prerequisite and is left alone. This is the safe case, and it is safe only because a delta holds
	//      deltas.
	//   3. The plan holds it but its scope was never determined. Nothing can say whether case 1 or case 2 applies,
	//      so the closure refuses rather than guessing in either direction.
	//
	// One function can arm or call another and a function can name itself, so the graph is CYCLIC. This walks a
	// worklist with a membership check rather than recursing: a unit already in the closed set is skipped, which
	// makes a cycle terminate and makes a self-reference a no-op rather than a missing prerequisite.
	void ClosePrerequisites(
		const TArray<FCrowdyApplyUnit>& AllUnits,
		const FCrowdyApplyPlanInput& Plan,
		const TArray<FString>& SelectedKeys,
		TArray<FCrowdyApplyUnit>& OutClosedSet,
		TArray<FCrowdyApplyAddition>& OutAdditions,
		TArray<FCrowdyApplyUnit>& OutUnresolvable);

	// THE PLAN. Everything above, assembled and judged.
	FCrowdyApplyPlan BuildPlan(const FCrowdyApplyPlanInput& Plan, const TArray<FString>& SelectedKeys);

	// The parts BuildPlan composes, exposed because each is a rule worth exercising alone.
	FCrowdyApplySheet BuildSheet(
		int64 AppId, const TArray<FCrowdyApplyUnit>& ClosedSet, const TArray<FCrowdyApplyAddition>& Additions,
		const TArray<FCrowdyApplyUnit>& Remainder, ECrowdyApplyRefusal Refusal,
		const TArray<FCrowdyApplyUnit>& Unresolvable);

	FString MakeConsentToken(int64 AppId, const TArray<FCrowdyApplyUnit>& ClosedSet, ECrowdyApplyRefusal Refusal);

	// Re-resolve a selection against a freshly planned unit set. Creating the app's session channel REBUILDS all
	// five pending arrays from a new diff, so a selection stored as indices is meaningless afterwards. Keys the new
	// plan does not hold are DROPPED, never mapped to a neighbour, and OutDropped names them so the caller can say
	// how many went.
	TArray<FString> ReconcileSelection(
		const TArray<FString>& SelectedKeys, const TArray<FCrowdyApplyUnit>& NewUnits,
		TArray<FString>& OutDropped);

	// Every fn:<name>(...) callee named anywhere in one function's expressions, in first-seen order, deduplicated
	// case-sensitively. The compiled function input carries no callee list, so the only place a cross-function call
	// survives is the expression text, and a closure that skipped it would send a function calling a function the
	// server does not have. The fields scanned are the ones CrowdyGameModelDelete::FunctionReferencesAttribute
	// scans, which is the one enumeration this codebase has of everywhere a function can name something.
	TArray<FString> FunctionCallees(const FCrowdyGameModelFunctionInput& Function);

	// Every attribute key one function writes, paired with the model that owns it where that can be told. A mutation
	// whose Target is "self" or empty writes a key on the function's own ContainerTypeName; a mutation whose Target
	// is a ref() writes a key on a model NOTHING in the struct names, so the model comes back empty and the closure
	// treats it as an unscoped key.
	TArray<TPair<FString /*OwningType, may be empty*/, FString /*Key*/>> FunctionWrites(
		const FCrowdyGameModelFunctionInput& Function);
}
