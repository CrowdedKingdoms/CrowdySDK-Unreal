// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Model/CrowdyStudioTypes.h" // FStudioContainerType, FStudioPropertyDef, FStudioFunction, FStudioAutomation, FStudioContainer

struct FCrowdyModelSnapshot;

// Turns the server's model schema reads into the rows the Models browser renders. Pure: no Slate, no HTTP,
// no UObject, no world, so it can be exercised without spinning up any of those.
//
// The vocabulary these rows are written in is fixed: a container type is a Model, a property definition is an
// Attribute, a live container is a Live model. The raw server nouns stay in the Advanced tab, which exists to
// show them.
//
// Every builder takes an optional snapshot of the last schema plan for the app. With one, each row is classified
// against the project and rows are also synthesized for what the project declares and the server does not have yet.
// Without one, nothing has been planned and nothing can be claimed: every row reads Unknown / None, which is a blank
// Source cell and a blank Status cell.

enum class ECrowdyModelRowKind : uint8
{
	Model,
	Attribute,
	Function,
	Automation,

	// One live container the server currently holds, as listed on the Live tab.
	LiveInstance
};

// Where a row came from. Unknown means nobody has planned this app yet, or the entity's only author was one the
// plan could not act on: in both cases the honest answer is to say nothing rather than guess.
enum class ECrowdyModelProvenance : uint8
{
	Unknown,
	CodeSynced,
	CodeNotPushed,
	CodeDrifted,
	ServerOnly,
	KitOwned
};

// How a row differs from the project. None is the healthy answer and the common one.
enum class ECrowdyModelDrift : uint8
{
	None,
	NotOnServerYet,
	ChangedInCode,
	OnlyOnServer,
	NeedsAFix,
	CannotBeChecked
};

// Which sources the browser is showing. InCode covers everything the project declares, whether or not the server has
// it yet; OnlyOnServer covers everything the project does not declare, kit-deployed schema included.
//
// Unknown provenance is visible under ALL THREE settings, not just All. A setting is a claim about where a row came
// from, and Unknown is precisely the case where no such claim can be made: hiding those rows would empty the whole
// page for an app nobody has planned yet, which reads as a failed load rather than as a filter doing its job.
enum class ECrowdyModelSourceFilter : uint8
{
	All,
	InCode,
	OnlyOnServer
};

// One entity's verdict against the project: where it came from, how it differs, and what declares it.
struct FCrowdyModelClassification
{
	ECrowdyModelProvenance Provenance = ECrowdyModelProvenance::Unknown;
	ECrowdyModelDrift Drift = ECrowdyModelDrift::None;

	// The declaring class or the authoring effect asset, as a path the editor can open. Empty when nothing in code
	// declares this entity.
	FString CodePath;
};

// One line in a section table: an attribute, a function, an automation, or a live instance of the selected
// model. One struct serves all four kinds because an unused FString costs nothing until something is put in
// it, so the cells only the live table fills stay free everywhere else.
struct FCrowdyModelRow
{
	ECrowdyModelRowKind Kind = ECrowdyModelRowKind::Attribute;

	// Verbatim from the server: the attribute key, the function name, the automation name, or the live
	// instance's id.
	FString Name;

	// The model this belongs to. Empty for an app-wide automation, which belongs to no model.
	FString OwningType;

	// Designer-facing label for the name column.
	FString Primary;

	// The name column's text when the row has a more readable form of its name than the server key. Today that is
	// an attribute, whose key is lowercased on the way to the server: the row shows the spelling the attribute was
	// authored under, or that spelling reconstructed from the key when nothing in the project declares it.
	//
	// Separate from Primary rather than replacing it, and empty on every row that has nothing better to show, so
	// the sort order and the search key stay written in one form. Read it through DisplayLabel, never directly.
	// A LABEL, never identity: Name is what addresses the entity on the server.
	FString Display;

	// Designer-facing detail: the value type, the return type, the trigger phrase, or the live instance's id.
	FString Secondary;

	// The description column. May be empty.
	FString Detail;

	// Who holds the live instance, already phrased for the column: "owner #42" or "unowned". Empty for every
	// other row kind.
	FString Owner;

	// The session the live instance belongs to. Empty for an app-global instance, and for every other row kind.
	FString Session;

	// The key the live instance was ensured under. Empty when it was created outright, and for every other row
	// kind.
	FString Binding;

	ECrowdyModelProvenance Provenance = ECrowdyModelProvenance::Unknown;
	ECrowdyModelDrift Drift = ECrowdyModelDrift::None;

	// The Source column's text and the Status column's word, spelled out once here rather than in a bound Slate
	// attribute: a bound lambda is evaluated every frame the pane is on screen, and neither of these can change
	// between frames. Both are empty when there is nothing to say, which is the common case.
	FString ProvenanceText;
	FString DriftText;

	// The class or effect asset that declares this row, as a path the editor can open. Empty when nothing in code
	// declares it.
	FString CodePath;

	// True for a row built from the project rather than from a server read, because the server has no such entity
	// yet. Such a row has nothing on the server to act on, so anything that reads or writes server state must skip it.
	bool bCodeOnly = false;

	// Lowercase haystack, built once when the row is built. Filtering never rebuilds it.
	FString SearchKey;

	// What to put in front of a reader for this row's name, which is Display when the row has one and Primary
	// otherwise. Every read-only surface calls this so none of them has to know which rows carry a Display.
	const FString& DisplayLabel() const { return Display.IsEmpty() ? Primary : Display; }
};

// One model in the left-hand list.
struct FCrowdyModelSummary
{
	FString TypeName;

	// DisplayName, falling back to TypeName when it is blank. A blank name cell is never acceptable.
	FString Display;

	FString Description;

	int32 FunctionCount = 0;
	int32 AutomationCount = 0;

	// INDEX_NONE means "not loaded yet". Attributes are read per model when it is selected, so a model nobody
	// has opened genuinely has no count to show, and 0 would claim it has none. A model that exists only in the
	// project is the exception: the server has nothing to read for it, so its count is known here and final.
	int32 AttributeCount = INDEX_NONE;

	ECrowdyModelProvenance Provenance = ECrowdyModelProvenance::Unknown;
	ECrowdyModelDrift Drift = ECrowdyModelDrift::None;

	// Precomputed exactly as on a row, and for the same reason.
	FString ProvenanceText;
	FString DriftText;

	// The class that declares this model. Empty when no class does.
	FString CodePath;

	// True for a model built from the project because the server does not have it yet. Its attributes are already
	// known (AttributeCount above is real), so nothing should be fetched for it.
	bool bCodeOnly = false;

	FString SearchKey;
};

namespace CrowdyModelLedger
{
	// The SDK provisions a revision attribute and a per-type touch function on every model so that a collection
	// change is observable. They are plumbing, not design surface, so they never become rows and never enter a
	// count. Both answers come from the SDK's own reserved-name predicates rather than a second copy of the
	// names here: a copy stops matching the day the SDK changes one, and the visible symptom is these entries
	// quietly reappearing in the browser with nothing to indicate why.
	bool IsReservedAttribute(const FString& Key);
	bool IsReservedFunction(const FString& Name);

	// Joins the parts a row is searched by into one lowercase haystack. Called once per row at build time; the
	// filter lowercases only the query and compares against the stored key.
	FString MakeSearchKey(const TArray<FString>& Parts);

	// One entity's verdict against the last plan. A null snapshot yields Unknown / None with no path, which is
	// exactly what every row read before anyone pressed Preview changes must say.
	//
	// Whether an entity is authored in code is answered from the snapshot's DESIRED schema, never by inverting the
	// plan's server-only lists: those already have kit protection and skipped-author exclusions folded in, so the
	// inversion is wrong in opposite directions for a kit type and for a type owned by an effect that will not
	// compile. The snapshot does not carry those lists at all.
	FCrowdyModelClassification ClassifyModel(const FCrowdyModelSnapshot* Snapshot, const FString& TypeName);
	FCrowdyModelClassification ClassifyAttribute(
		const FCrowdyModelSnapshot* Snapshot, const FString& TypeName, const FString& Key);
	FCrowdyModelClassification ClassifyFunction(
		const FCrowdyModelSnapshot* Snapshot, const FString& TypeName, const FString& Name);
	FCrowdyModelClassification ClassifyAutomation(const FCrowdyModelSnapshot* Snapshot, const FString& Name);

	// The model list, with each model's function and automation counts. Reserved functions are excluded from
	// FunctionCount: a model whose only function is its touch function would otherwise read "1 function" and
	// open onto an empty table, which is indistinguishable from a failed read.
	//
	// With a snapshot, a model the project declares and the server does not have yet becomes a model in this list,
	// and every model's counts include the functions and automations that are likewise not on the server yet, so the
	// count a model shows here is the number of rows it opens onto.
	TArray<FCrowdyModelSummary> BuildModelList(
		const TArray<TSharedPtr<FStudioContainerType>>& Types,
		const TArray<TSharedPtr<FStudioFunction>>& Functions,
		const TArray<TSharedPtr<FStudioAutomation>>& Automations,
		const FCrowdyModelSnapshot* Snapshot = nullptr);

	// OwningTypeName is the model whose attributes these are, needed because a model that exists only in the project
	// has no server attributes to read the name off. Left empty it is taken from the defs, which answers for every
	// model the server does have.
	TArray<FCrowdyModelRow> BuildAttributeRows(
		const TArray<TSharedPtr<FStudioPropertyDef>>& Defs,
		const FCrowdyModelSnapshot* Snapshot = nullptr,
		const FString& OwningTypeName = FString());

	TArray<FCrowdyModelRow> BuildFunctionRows(
		const TArray<TSharedPtr<FStudioFunction>>& Functions,
		const FString& TypeName,
		const FCrowdyModelSnapshot* Snapshot = nullptr);

	// The automations attributed to TypeName. An empty TypeName returns the app-wide automations, the ones that
	// name no target type: they belong to no model and must still be reachable somewhere.
	TArray<FCrowdyModelRow> BuildAutomationRows(
		const TArray<TSharedPtr<FStudioAutomation>>& Automations,
		const TArray<TSharedPtr<FStudioAutomationTrigger>>& Triggers,
		const FString& TypeName,
		const FCrowdyModelSnapshot* Snapshot = nullptr);

	// The live instances of one model, in the order the server answered in. That order is the one paging is
	// defined over, so the rows are deliberately not re-sorted here: sorting them would reorder a page the
	// server already fixed, and appending the next page would then reshuffle rows the reader is looking at.
	// A type name is a server key, so the match is case-sensitive.
	TArray<FCrowdyModelRow> BuildLiveRows(const TArray<TSharedPtr<FStudioContainer>>& Containers, const FString& TypeName);

	// A live instance's property values as one readable line: "hp 84, mana 50, stamina 100, level 7". Beyond
	// MaxEntries it stops and says how many it left out, so the result has a bounded length whatever the
	// instance carries; it renders on a single line in a strip that does not scroll. Empty or unparseable JSON
	// yields an empty string rather than an error or the raw input, because the strip has no room to explain
	// itself and the raw JSON is exactly what this replaces.
	FString FormatPropertySummary(const FString& PropertiesJson, int32 MaxEntries = 6);

	// Whether one provenance survives a source setting. Unknown survives all three; see ECrowdyModelSourceFilter.
	bool MatchesSourceFilter(ECrowdyModelProvenance Provenance, ECrowdyModelSourceFilter Filter);

	// Case-insensitive substring match against the prebuilt search key, narrowed to one source setting. The two
	// compose: a row must satisfy both, so typing a query never widens what the source setting shows and picking a
	// source setting never discards the query. An empty or whitespace-only query returns everything the source
	// setting allows, in the input's order, which is already alphabetical.
	TArray<FCrowdyModelSummary> FilterModels(
		const TArray<FCrowdyModelSummary>& Models,
		const FString& Query,
		ECrowdyModelSourceFilter Filter = ECrowdyModelSourceFilter::All);
	TArray<FCrowdyModelRow> FilterRows(
		const TArray<FCrowdyModelRow>& Rows,
		const FString& Query,
		ECrowdyModelSourceFilter Filter = ECrowdyModelSourceFilter::All);
}
