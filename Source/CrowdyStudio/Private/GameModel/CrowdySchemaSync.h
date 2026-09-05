// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdyContainerScan.h" // UCrowdySchemaScanSettings, FCrowdyScannedAsset, FCrowdyContainerScanPlan
#include "Model/CrowdyStudioTypes.h" // FStudioContainerType, FStudioPropertyDef, FStudioFunction, FStudioAutomation
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h" // FCrowdyGameModelFunctionInput
#include "Replication/GameModel/Effect/CrowdyGameModelAutomationInput.h" // FCrowdyGameModelAutomationInput, ...Trigger...
#include "Templates/Function.h"

class UClass;
class UObject;
class FProperty;
class FCrowdyEffectPlanCache;
struct FCrowdyEffectGatherContext;

/**
 * The desired server property definition reflected from one meta=(CrowdyModel) UPROPERTY: its server key,
 * value type, default value, and read-visibility, ready to diff against the live server schema. The server key
 * is the lowercased property name unless a CrowdyKey meta overrides it.
 */
struct FCrowdyDesiredPropertyDef
{
	// The UPROPERTY spelling the attribute was authored under, e.g. "CropStage". Deriving the server key
	// lowercases it, so this is the only place the authored capitalization survives a reflection pass, and it is
	// carried so a read-only surface can show a name a designer recognizes. A LABEL, never identity: nothing the
	// server is addressed by is derived from it. Empty for a def no class declared.
	FName PropertyName;

	FString Key;
	FString ValueType;        // "int" | "float" | "bool" | "string"
	FString DefaultValueJson; // canonical JSON text of the CDO default
	FString Visibility = TEXT("public");
	FString Writable = TEXT("function");

	// The attribute's native ClampMin/ClampMax. The server schema has no clamp concept, so these are never
	// upserted and never diffed; they are reflected here because an effect's compiled assignment wraps itself in
	// them, which makes them part of what a cached compile result depends on.
	bool bHasClamp = false;
	double ClampMin = 0.0;
	double ClampMax = 0.0;

	// The authored spelling as text, and EMPTY when none was recorded. Every reader goes through this rather than
	// calling ToString: a default FName stringifies as "None", which would otherwise reach a label as if it were
	// the name somebody wrote.
	FString AuthoredName() const { return PropertyName.IsNone() ? FString() : PropertyName.ToString(); }
};

/** The desired container type reflected from one meta=(CrowdyContainer="Type") class + its attributes. */
struct FCrowdyDesiredContainerType
{
	FString TypeName;
	FString DisplayName;
	FString InstantiableBy = TEXT("member");
	FString DefaultVisibility = TEXT("public");
	TArray<FCrowdyDesiredPropertyDef> Props;
	// The class that declared this type, as an FSoftClassPath string, so a reader can open the declaring asset or
	// navigate to the C++ class. Empty when the type was not reflected from a class (the SDK's own reserved
	// collection plumbing appends types that no class declares).
	FString OwningClassPath;
};

/**
 * One planned gameModelUpsertContainerType.
 *
 * bIsNew is NOT report-only. It says whether this op CREATES the entity or updates one the server already has, and
 * a selective apply turns on exactly that: an entity the server already has is not a prerequisite of anything, so
 * it is left out of a closure, while a create must be dragged in behind whatever needs it. Changing what this
 * means changes what a partial apply writes.
 */
struct FCrowdySchemaTypeUpsert
{
	FCrowdyDesiredContainerType Type;
	bool bIsNew = false;
};

/** One planned gameModelUpsertPropertyDef. bIsNew decides prerequisite-hood; see FCrowdySchemaTypeUpsert. */
struct FCrowdySchemaPropUpsert
{
	FString ContainerTypeName;
	FCrowdyDesiredPropertyDef Prop;
	bool bIsNew = false;
};

/** A reference to one server property def (container type + key), used for the opt-in prune candidates. */
struct FCrowdySchemaPropRef
{
	FString ContainerTypeName;
	FString Key;
};

/**
 * A reference to one server function, carrying the container type the diff identified it by rather than the bare
 * name the delete mutation happens to take. The diff's identity for a function is the (container type, name) pair,
 * so two container types carrying a function of the same name are two distinct functions; a bare name cannot tell
 * them apart, and everything downstream of the diff would then have to guess which one a candidate belongs to.
 *
 * ContainerTypeName is EMPTY when the read this was built from did not say which container type owns the function.
 * That is deliberately NOT read as "no container type owns it": a server record with no binding and a read that
 * never carried one are indistinguishable here, and only one of those two is safe to act on. HasOwningType is
 * spelled out so a call site cannot mistake an empty string for a determined answer, and a caller that names the
 * owning model to a user has to answer for the undetermined case rather than print nothing and imply app-wide.
 *
 * Carrying the type is NOT a claim that gameModelDeleteFunction is scoped by it: that mutation takes only
 * (appId, name). The type is here so the phrasing, the refresh and any later marking of this candidate name the
 * right model.
 */
struct FCrowdySchemaFunctionRef
{
	FString ContainerTypeName;
	FString Name;

	bool HasOwningType() const { return !ContainerTypeName.IsEmpty(); }
};

/**
 * One name an effect asset claims, together with the scope the server resolves that name in. Scope is the
 * container type name for a function (the server scopes a model function by its container type, so the same
 * function name on two different types is two distinct functions) and EMPTY for a name the server keeps unique
 * app-wide, such as an automation name. Input to FCrowdySchemaSync::DetectDuplicateNames.
 */
struct FCrowdySchemaNameCandidate
{
	FString AssetPath;
	FString Scope;
	FString Name;
};

/**
 * One (scope, name) pair claimed by two or more Crowdy Effect assets in a single plan: a conflict a sync must
 * never resolve by picking a winner, since which asset "wins" would depend on asset-registry sweep order (not the
 * designer), so the loser's edits would silently vanish from the server. See
 * FCrowdySchemaSync::DetectDuplicateNames.
 */
struct FCrowdySchemaNameConflict
{
	FString Scope; // the container type for a function conflict; empty for an app-wide name
	FString Name;
	TArray<FString> AssetPaths; // every effect asset claiming this pair, in input order; always 2 or more
};

/**
 * Which asset declared one entity a plan looked at, recorded for EVERY effect asset the plan visited including the
 * ones it passed over, so a reader can answer "where did this come from" for a name that never reached the server.
 * Scope is the container type name for a function and EMPTY for an automation, matching ScopedNameKey.
 *
 * bSkipped means an effect asset claims this name but was NOT compiled into the plan: an unmigrated text effect, a
 * compile error, an empty or reserved name, or exclusion as a duplicate. bConflicted means two or more effect
 * assets claim this exact (Scope, Name) pair, and is set on every author of the pair rather than on the later ones,
 * since neither is synced.
 */
struct FCrowdySchemaAuthorship
{
	FString Scope;
	FString Name;
	FString AssetPath;
	bool bSkipped = false;
	bool bConflicted = false;
};

/**
 * One effect asset's compile facts, as a plan sees them: everything both gathers need from the asset, and nothing
 * that depends on the app being synced. Plain data with no UObject reference, so it survives across frames and can
 * be reused by a later plan without re-loading the asset.
 *
 * Deliberately pre-decision: the record states what the effect compiled to, and each gather applies its own
 * acceptance rules to it. That is what lets one sweep serve both, and what makes those rules testable without
 * loading an asset.
 */
struct FCrowdyEffectPlanRecord
{
	FString AssetPath;
	// The container type the effect's function lives on, resolved even when the effect was passed over, so a
	// skipped effect can still be reported against the model it belongs to.
	FString TargetTypeName;
	// The container type the effect declares as the source of a cross-type write, empty when the source shares the
	// target's type. Recorded because the compile validates every source.<attr> reference against THIS type's
	// attributes, which makes it part of the content key a stored compile result may be reused under.
	FString SourceTypeName;
	// The effective function name, recorded before any skip, so a passed-over effect's live server function is
	// never mistaken for an orphan.
	FString EffectiveFunctionName;

	bool bCompileFailed = false;
	FString FirstCompileError;          // the first Error diagnostic, for the warning line

	FCrowdyGameModelFunctionInput Function; // meaningful only when the compile succeeded
	bool bHasAutomation = false;
	FCrowdyGameModelAutomationInput Automation;
	bool bHasTrigger = false;
	FCrowdyGameModelAutomationTriggerInput Trigger;

	// The content key this record was compiled under, and whether it may be reused at all. See
	// FCrowdyEffectPlanCache.
	FString PackageHash;
	FString VocabularyHash;
	bool bReusable = false;
};

/**
 * One planned gameModelUpsertFunction. The desired function is the neutral input a UCrowdyEffect compiled to via
 * FCrowdyEffectLowering; the sync sends it field-for-field, including the model-changed notifications the effect
 * authors. bIsNew decides prerequisite-hood; see FCrowdySchemaTypeUpsert.
 */
struct FCrowdySchemaFunctionUpsert
{
	FCrowdyGameModelFunctionInput Function;
	bool bIsNew = false;
};

/** One planned gameModelUpsertAutomation. bIsNew decides prerequisite-hood; see FCrowdySchemaTypeUpsert. */
struct FCrowdySchemaAutomationUpsert
{
	FCrowdyGameModelAutomationInput Automation;
	bool bIsNew = false;
};

/**
 * One planned gameModelUpsertAutomationTrigger. bIsNew decides prerequisite-hood; see FCrowdySchemaTypeUpsert. A
 * trigger create and a trigger update are one op either way, so nothing else reads this one.
 */
struct FCrowdySchemaTriggerUpsert
{
	FCrowdyGameModelAutomationTriggerInput Trigger;
	bool bIsNew = false;
};

/**
 * The structural diff result: the upserts to apply (deltas only), non-destructive warnings, and the server-only
 * entities that exist on the server but are NOT declared in code. The sync never deletes those on its own; they
 * are the candidates for the explicit, opt-in prune.
 */
struct FCrowdySchemaDelta
{
	TArray<FCrowdySchemaTypeUpsert> TypeUpserts;
	TArray<FCrowdySchemaPropUpsert> PropUpserts;
	TArray<FCrowdySchemaFunctionUpsert> FunctionUpserts;
	TArray<FCrowdySchemaAutomationUpsert> AutomationUpserts;
	TArray<FCrowdySchemaTriggerUpsert> TriggerUpserts;
	TArray<FString> Warnings;
	TArray<FString> ServerOnlyTypes;              // container types on the server but not in code
	TArray<FCrowdySchemaPropRef> ServerOnlyProps; // props on a code-owned type but not in code
	// Functions on the server not authored by any effect, each carrying the container type the diff identified it
	// by. Scoped rather than bare, because a bare name leaves everything downstream guessing which container type
	// a candidate belongs to, and a prune list is read by a user deciding what to destroy.
	TArray<FCrowdySchemaFunctionRef> ServerOnlyFunctions;
	TArray<FString> ServerOnlyAutomations;        // automations on the server not authored by any effect

	// Whether a function of this exact name is a server-only candidate, on any container type. Case-SENSITIVE:
	// FString::operator== folds case, and two server keys differing only in case are two distinct functions.
	bool HasServerOnlyFunction(const FString& Name) const
	{
		return ServerOnlyFunctions.ContainsByPredicate(
			[&Name](const FCrowdySchemaFunctionRef& Ref) { return Ref.Name.Equals(Name, ESearchCase::CaseSensitive); });
	}

	int32 UpsertCount() const { return TypeUpserts.Num() + PropUpserts.Num() + FunctionUpserts.Num() + AutomationUpserts.Num() + TriggerUpserts.Num(); }
	int32 ServerOnlyCount() const { return ServerOnlyTypes.Num() + ServerOnlyProps.Num() + ServerOnlyFunctions.Num() + ServerOnlyAutomations.Num(); }
	bool IsEmpty() const { return TypeUpserts.Num() == 0 && PropUpserts.Num() == 0 && FunctionUpserts.Num() == 0 && AutomationUpserts.Num() == 0 && TriggerUpserts.Num() == 0; }
};

/** A view-facing summary of a plan (dry run) or an applied sync: counts, per-entity lines, and warnings. */
struct FCrowdySchemaSyncReport
{
	int32 TypesToCreate = 0;
	int32 TypesToUpdate = 0;
	int32 PropsToCreate = 0;
	int32 PropsToUpdate = 0;
	int32 FunctionsToCreate = 0;
	int32 FunctionsToUpdate = 0;
	int32 AutomationsToCreate = 0;
	int32 AutomationsToUpdate = 0;
	int32 TriggersToCreate = 0;
	int32 TriggersToUpdate = 0;
	int32 ServerOnlyTypeCount = 0;       // server-only types (prune candidates)
	int32 ServerOnlyPropCount = 0;       // server-only props (prune candidates)
	int32 ServerOnlyFunctionCount = 0;   // server-only functions (prune candidates)
	int32 ServerOnlyAutomationCount = 0; // server-only automations (prune candidates)
	TArray<FString> Lines;    // one human-readable line per planned upsert
	TArray<FString> Warnings;
	bool bApplied = false;    // false = a dry-run plan; true = the upserts were written
	bool bValid = false;      // true once a plan has been computed
	// A durable outcome banner the report panel renders above the counts: set when a plan read failed (so the
	// panel says "re-plan" instead of silently keeping a stale plan) or when an apply stopped partway ("Applied
	// N of M ... re-apply to finish"). Empty on a clean plan/apply. Distinct from the ephemeral status line,
	// which carries the specific server error; this states what to do next.
	FString StatusNote;

	int32 UpsertCount() const { return TypesToCreate + TypesToUpdate + PropsToCreate + PropsToUpdate + FunctionsToCreate + FunctionsToUpdate + AutomationsToCreate + AutomationsToUpdate + TriggersToCreate + TriggersToUpdate; }
	int32 ServerOnlyCount() const { return ServerOnlyTypeCount + ServerOnlyPropCount + ServerOnlyFunctionCount + ServerOnlyAutomationCount; }
};

/**
 * Reflects meta=(CrowdyContainer) classes' meta=(CrowdyModel) attributes into the Game Model schema and
 * structurally diffs them against the live server schema so only real deltas are upserted. PURE: no GraphQL,
 * no I/O, no world -- the async read/apply lives in FCrowdyStudioController::SyncSchemaFromCode. This split
 * keeps the diff (the design-sensitive, idempotency-critical part) unit-testable without HTTP.
 *
 * Idempotency: the three gameModelUpsert* ops are create-or-update on their natural key, so a re-sync with
 * no code change must produce ZERO upserts. Every field is emitted explicitly (never omitted) so the server
 * read-back matches; JSON-typed defaults are compared SEMANTICALLY (parse then canonical-compare) because
 * the server makes no byte-stability guarantee (100 vs 100.0, key order).
 */
class FCrowdySchemaSync
{
public:
	// Every loaded, non-transient class carrying a CrowdyContainer tag. The tag alone declares a container: a
	// container whose whole contribution is functions (signals, timers, automations) declares no attribute at all,
	// and it still needs its type on the server for those functions to bind to.
	//
	// This reads LIVE classes and nothing else, which is what makes a resident object always win over anything an
	// asset-registry tag claimed about it. Its answer is the union of the native containers below and whatever
	// Blueprint containers the scan brought resident, so a caller runs the scan first and this second.
	static TArray<UClass*> GatherContainerClasses();

	// Every RESIDENT NATIVE class carrying a CrowdyContainer tag. A native class has no asset behind it, so it has
	// no asset-registry entry and no tag can ever describe it; iterating loaded classes is the only way to reach
	// one, and native classes are resident from module startup, so it always works. This is a required half of the
	// scan rather than a fallback, and it is why the registry sweep below only has to answer for Blueprints.
	static TArray<UClass*> GatherNativeContainerClasses();

	// ResolveScannedRoots, IsPathUnderAnyRoot, PartitionScannedAssets and BuildContainerScanPlan now live on
	// FCrowdyContainerScan (CrowdyContainerScan.h, Public): the registry-only half of this scan that an
	// editor-only tool outside CrowdyStudio needs to call without depending on the rest of the schema-diff types.

	// Brings the plan's assets into memory, then calls OnComplete on the game thread. Streamed rather than
	// force-loaded, because this runs behind the most-used button in the Studio and the synchronous form held the
	// game thread for seconds. OnComplete ALWAYS fires, including when there is nothing to load and when the
	// request is refused, so a plan waiting on it is never stranded.
	static void StreamContainerScanPlan(const FCrowdyContainerScanPlan& Plan, TFunction<void()> OnComplete);

	// The gather's acceptance rule over an explicit candidate list, split out so it is testable without a fixture
	// that a live sync could reach. bExcludeTestContainers is always true in production; a test passes false so a
	// meta=(CrowdyContainerTest) fixture can stand in for a real container class, since GatherContainerClasses is
	// the only production caller and it always excludes them.
	static TArray<UClass*> SelectContainerClasses(const TArray<UClass*>& Candidates, bool bExcludeTestContainers);

	// Reflect a class set into desired container types, deduped by type name (a duplicate tag -> warning).
	static TArray<FCrowdyDesiredContainerType> BuildDesiredSchema(const TArray<UClass*>& Classes, TArray<FString>& OutWarnings);

	// Reflect one class into its desired container type + property defs. Empty TypeName when the class
	// carries no CrowdyContainer tag (the caller skips it).
	static FCrowdyDesiredContainerType BuildDesiredForClass(const UClass* Class);

	// Serialize a scalar/string UPROPERTY's CDO value to canonical defaultValueJson text. Empty for an
	// unsupported type (mirrors MapPropertyToValueType's supported leaves: int/float/bool/string/enum).
	static FString PropertyDefaultToJson(const FProperty* Property, const UObject* CDO);

	// The pure structural diff. CurrentPropsByType maps a server type name -> its server property defs.
	// Emits an upsert only for an added/changed entity; a server-only type/prop and a value-type change
	// each emit a warning (the SDK never auto-deletes server state). A server type whose name starts with a
	// RecognizedKitTypePrefixes entry (a deployed Game Kit's type prefix) is kept off the server-only prune list
	// and left in place with a warning: the kit owns that whole namespace, so a schema sync must not offer it for
	// prune. The prefix match is case-sensitive on the type-name start; an empty prefix never matches.
	// RecognizedKitTypeNames is the exact-name layer for that same protection: a server type whose full name is in
	// the set (the names a deploy actually seeded, persisted at deploy time) is likewise kept off the prune list.
	// This is what protects a kit whose type prefix is empty, whose bare type names ("Combatant") no prefix rule
	// could recognize. The two layers are additive: a type protected by either is left in place.
	static FCrowdySchemaDelta DiffSchema(
		const TArray<FCrowdyDesiredContainerType>& Desired,
		const TArray<FStudioContainerType>& CurrentTypes,
		const TMap<FString, TArray<FStudioPropertyDef>>& CurrentPropsByType,
		const TSet<FString>& RecognizedKitTypePrefixes = TSet<FString>(),
		const TSet<FString>& RecognizedKitTypeNames = TSet<FString>());

	// True when TypeName starts with any recognized kit type prefix (case-sensitive, PascalCase prefix match on the
	// name start). An empty prefix never matches, since it would swallow the entire server schema. Pure + static.
	static bool IsRecognizedKitTypeName(const FString& TypeName, const TSet<FString>& RecognizedTypePrefixes);

	// True when FunctionName starts with a recognized kit's function-name prefix, i.e. toSnakeCase(prefix) + "_" for
	// any recognized type prefix (kit functions are named from the snake-cased type prefix). An empty prefix never
	// matches. Pure + static.
	static bool IsRecognizedKitFunctionName(const FString& FunctionName, const TSet<FString>& RecognizedTypePrefixes);

	// The kit's PascalCase/camelCase -> snake_case transform, replicated exactly from the vendored kit's toSnakeCase
	// so a derived function-name prefix matches without a cross-module dependency on the bridge. Pure + static.
	static FString ToKitSnakeCase(const FString& Name);

	// Semantic equality of two JSON-value texts: parse each and canonical-compare, so a formatting
	// difference (100 vs 100.0, reordered object keys) does not read as drift. Empty-vs-empty is equal;
	// empty-vs-present is not. Falls back to a trimmed text compare when a side is not valid JSON.
	static bool JsonValueEquals(const FString& A, const FString& B);

	// Semantic equality for an invoke policy specifically. The server compiles a stored policy and returns it with
	// the compiled "ast" object written into the same JSON, so a plain semantic compare of authored-vs-readback is
	// permanent false drift for every function carrying a policy (the SDK's own __crowdy_touch functions and every
	// owner-gated effect). Strips the server-computed key from both sides, then compares as JsonValueEquals does.
	// Empty-vs-present stays a real difference: an empty send explicitly clears the server's policy.
	static bool InvokePolicyEquals(const FString& Desired, const FString& Current);

	// Format a delta (+ any pre-diff warnings, e.g. duplicate tags) into a view-facing report.
	static FCrowdySchemaSyncReport BuildReport(const FCrowdySchemaDelta& Delta, const TArray<FString>& ExtraWarnings, bool bApplied);

	// Pure: groups candidates by their (Scope, Name) pair and returns only the pairs claimed by two or more assets,
	// each conflict carrying every authoring asset path in input order (a pair with a single author is not a
	// conflict and is omitted). Grouping on the pair is what keeps a legitimate per-container naming convention
	// working: two container types may each declare a "take_damage", because those are two distinct server
	// functions. An app-wide name (an automation) passes an empty Scope, which groups on the name alone.
	// GatherDesiredFunctions and GatherDesiredAutomations both call this before adding anything to their desired
	// list, so a duplicate is excluded from the plan entirely -- neither authoring asset is synced -- instead of
	// one silently winning on sweep order. One linear pass over the input (a single TMap grouping); the caller's
	// asset-registry sweep, not this function, is the expensive step.
	static TArray<FCrowdySchemaNameConflict> DetectDuplicateNames(const TArray<FCrowdySchemaNameCandidate>& Candidates);

	// The lookup/grouping key for a name the server resolves within a scope, e.g. a model function within its
	// container type. An empty scope (an app-wide name such as an automation) keys on the name alone. Distinct
	// (scope, name) pairs always produce distinct keys. Pure + static; used by DetectDuplicateNames, by
	// DiffFunctions' current-function lookup, and by the gathers' exclusion sets, so all of them agree on identity.
	static FString ScopedNameKey(const FString& Scope, const FString& Name);

	// Marks a warning line as a hard duplicate-name conflict so BuildReport (via SplitDuplicateConflictWarnings)
	// promotes it into the report's durable StatusNote banner instead of leaving it buried in the ordinary warning
	// list. GatherDesiredFunctions / GatherDesiredAutomations call this when reporting a DetectDuplicateNames
	// conflict; a marked warning string is the only channel available to those steps, since they run before a
	// FCrowdySchemaDelta exists to carry a typed field. Pure + static.
	static FString MarkDuplicateConflictWarning(const FString& Message);

	// Splits InWarnings into the ordinary warnings the report panel lists (OutPlainWarnings, marker stripped) and,
	// when one or more entries were marked by MarkDuplicateConflictWarning, a durable OutStatusNote banner naming
	// every conflict -- the same mechanism FCrowdyStudioController::FailSchemaPlan uses for a failed read, so a
	// duplicate name is impossible to miss in the Studio panel rather than sitting quietly in the warning list.
	// OutStatusNote is left empty when nothing was marked. Pure + static; BuildReport is the sole caller.
	static void SplitDuplicateConflictWarnings(
		const TArray<FString>& InWarnings, TArray<FString>& OutPlainWarnings, FString& OutStatusNote);

	// Every UCrowdyEffect asset in the project, compiled to its neutral function input. A compile error (an
	// unshippable effect) becomes a warning and that function is skipped -- the sync never upserts a broken
	// function. Two effects authoring the same function name ON THE SAME CONTAINER TYPE is a hard conflict
	// (DetectDuplicateNames): a MarkDuplicateConflictWarning warning names both, and NEITHER function is added to
	// the desired list, so a duplicate can never silently override or be silently dropped in favor of the other.
	// The same name on two different container types is not a conflict at all, since the server scopes a function
	// by its container type and those are two distinct functions. This scans + loads assets
	// (editor only), so it is NOT the pure diff; DiffFunctions below is the testable core. OutRecognizedNames
	// collects the effective function name of EVERY effect asset (including the skipped ones), so DiffFunctions can
	// keep a skipped effect's live server function off the prune list instead of mistaking it for an orphan.
	// OutAuthorship receives one entry per effect asset visited, INCLUDING every skipped one, so a caller can say
	// which asset declared each function.
	// Context is the plan's shared sweep state: the container vocabulary this plan reflected, the cross-plan compile
	// cache, and the assets the caller already streamed. Passing nullptr keeps the standalone behaviour (sweep and
	// load everything, cache nothing), which is what a test wants.
	static TArray<FCrowdyGameModelFunctionInput> GatherDesiredFunctions(
		TArray<FString>& OutWarnings,
		TSet<FString>& OutRecognizedNames,
		TArray<FCrowdySchemaAuthorship>& OutAuthorship,
		FCrowdyEffectGatherContext* Context = nullptr);

	// Whether a compiled function would do nothing at all if it were invoked: no writes, no timers, no notifications
	// and no return value. Upserting one REPLACES whatever the server holds under that name with an empty body, and a
	// caller then gets success with no state change, so both sync paths refuse it. An effect whose only job is a
	// require gate plus a timer is not empty by this test, and neither is one that only answers with a value.
	static bool FunctionDoesNothing(const FCrowdyGameModelFunctionInput& Function);

	// The pure half of GatherDesiredFunctions: given one compile record per effect asset, apply the acceptance rules
	// (compile error, empty name, reserved name, compiles to nothing, duplicate (container type, name) pair) and
	// produce the desired functions, their warnings, the recognized-name set, and one authorship entry per record. No
	// assets, no loading, no registry -- so every one of those rules is testable from plain records.
	static TArray<FCrowdyGameModelFunctionInput> SelectDesiredFunctions(
		const TArray<FCrowdyEffectPlanRecord>& Records,
		TArray<FString>& OutWarnings,
		TSet<FString>& OutRecognizedNames,
		TArray<FCrowdySchemaAuthorship>& OutAuthorship);

	// The pure structural diff of desired functions (compiled from effects) against the server's current
	// functions, appended onto InOutDelta. Identity is the (container type, function name) pair, matching how the
	// server scopes a model function; a desired function whose container type changed still matches the single
	// server function of that name, so a rebind reads as an update rather than stranding the old definition.
	// Create when absent, update when any authored field differs (containerTypeName / description / returnType / returnExpression / invokeScope,
	// invokePolicyJson compared SEMANTICALLY, parameters by name, mutations in order, notifications order-
	// independently). The SDK's own model-changed notification is AUTHORED (its destination stamped beforehand by
	// InjectSessionChannelTarget) while a seed/console-authored (non-SDK) notification is PRESERVED; an effect's
	// channel notification the sync cannot address (or a spatial notification, not authored yet) leaves the server's
	// notifications untouched and warns. A server function no effect authors is a server-only prune candidate --
	// EXCEPT one whose name is in RecognizedFunctionNames (a function some effect asset owns but that was skipped
	// this plan, e.g. an unmigrated or non-compiling effect); such a function is left in place and never offered for
	// prune, so a transiently-skipped effect can never lose its live server function.
	// A server function whose name starts with a RecognizedKitTypePrefixes entry's function prefix (a deployed Game
	// Kit's toSnakeCase(prefix)_ functions) is likewise kept off the prune list and left in place, so a schema sync
	// never offers a kit-deployed function for prune.
	// RecognizedKitFunctionNames is the exact-name layer for that protection: a server function whose full name is in
	// the set (the function names a deploy actually seeded, persisted at deploy time) is kept off the prune list.
	// This protects a kit whose type prefix is empty, whose bare function names ("attack", "advance_time") carry no
	// recognizable prefix. Additive to the prefix / container-type layers: a function protected by any is left in place.
	static void DiffFunctions(
		const TArray<FCrowdyGameModelFunctionInput>& Desired,
		const TArray<FStudioFunction>& CurrentFunctions,
		FCrowdySchemaDelta& InOutDelta,
		const TSet<FString>& RecognizedFunctionNames = TSet<FString>(),
		const TSet<FString>& RecognizedKitTypePrefixes = TSet<FString>(),
		const TSet<FString>& RecognizedKitFunctionNames = TSet<FString>());

	// Compute a schema-sync plan scoped to a SINGLE effect: its compiled function plus its one target container type,
	// diffed against the server snapshot for just those two entities. Composes DiffSchema (on the one desired type) and
	// DiffFunctions (on the one desired function), scoping the server snapshot to those names so no unrelated server
	// type or function is ever seen as an orphan. The server-only / prune lists are ALWAYS left empty: a per-asset sync
	// creates or updates that one effect and never deletes any server state. Pure (no HTTP/world); the async read + apply
	// lives in CrowdyStudioSyncService. A DesiredType with an empty TypeName, or a DesiredFunction with an empty Name
	// (an effect that could not be resolved or compiled), is simply skipped and yields no upsert -- the caller treats
	// that as "cannot sync", not "in sync". OutDelta receives the same deltas the returned report summarizes.
	static FCrowdySchemaSyncReport PlanForSingleEffect(
		const FCrowdyDesiredContainerType& DesiredType,
		const FCrowdyGameModelFunctionInput& DesiredFunction,
		const TArray<FStudioContainerType>& CurrentTypes,
		const TMap<FString, TArray<FStudioPropertyDef>>& CurrentPropsByType,
		const TArray<FStudioFunction>& CurrentFunctions,
		const TArray<FString>& ExtraWarnings,
		FCrowdySchemaDelta& OutDelta);

	// Every UCrowdyEffect asset that opts into running automatically, compiled to its neutral automation input (and
	// its optional event trigger). Scans + loads assets (editor only), mirroring GatherDesiredFunctions: a compile
	// error becomes a warning and that automation is skipped; two effects authoring the same automation name is a
	// hard conflict (DetectDuplicateNames) -- a MarkDuplicateConflictWarning warning names both, and NEITHER
	// automation (nor its trigger) is added to the desired list. An automation name is unique app-wide on the
	// server, so unlike a function name it is NOT scoped by container type. An automation is also dropped when the
	// function it runs is one GatherDesiredFunctions excludes (a duplicate function name, a reserved name, an empty
	// name): the two gathers exclude on different names, so without this cross-check an automation could survive
	// into the plan pointing at a function the plan never creates. OutRecognizedAutomationNames collects a best-effort
	// superset of every effect's automation
	// name (an effect's automation defaults to its function name, recorded before any skip), so DiffAutomations can
	// keep a transiently-skipped effect's live server automation off the prune list instead of mistaking it for an
	// orphan. OutTriggers collects the event triggers the gathered automations authored. Not the pure diff (it loads
	// assets); DiffAutomations below is the testable core. OutAuthorship receives one entry per effect asset that
	// authors an automation, INCLUDING every skipped one, with an EMPTY scope (an automation name is unique app-wide).
	// Context is the same shared sweep state GatherDesiredFunctions takes; passing the one the function gather already
	// used means this gather loads nothing at all.
	static TArray<FCrowdyGameModelAutomationInput> GatherDesiredAutomations(
		TArray<FString>& OutWarnings,
		TSet<FString>& OutRecognizedAutomationNames,
		TArray<FCrowdyGameModelAutomationTriggerInput>& OutTriggers,
		TArray<FCrowdySchemaAuthorship>& OutAuthorship,
		FCrowdyEffectGatherContext* Context = nullptr);

	// The pure half of GatherDesiredAutomations, mirroring SelectDesiredFunctions: the same compile records in, the
	// desired automations, their triggers, warnings, recognized names and authorship out. No assets, no loading.
	static TArray<FCrowdyGameModelAutomationInput> SelectDesiredAutomations(
		const TArray<FCrowdyEffectPlanRecord>& Records,
		TArray<FString>& OutWarnings,
		TSet<FString>& OutRecognizedAutomationNames,
		TArray<FCrowdyGameModelAutomationTriggerInput>& OutTriggers,
		TArray<FCrowdySchemaAuthorship>& OutAuthorship);

	// The pure structural diff of desired automations (compiled from effects) + their event triggers against the
	// server's current ones, appended onto InOutDelta. Identity is the automation Name: create when absent, update
	// when any authored field differs (paramsJson / selectorJson compared SEMANTICALLY via JsonValueEquals; the
	// budget ints, the enum-like strings, functionName / targetMode / targetTypeName / sessionId / intervalMs /
	// cronExpr / enabled by value). Triggers are matched by (automationName, onEvent) plus every filter; a debounce
	// change is an update, a new filter combination is a create (this slice never prunes a server-only trigger).
	// A server automation no effect authors is a server-only prune candidate -- EXCEPT one whose name is in
	// RecognizedAutomationNames (owned by an effect skipped this plan), whose exact name is in
	// RecognizedKitAutomationNames (a deploy actually seeded it), or whose name carries a deployed Game Kit's
	// function-name prefix (an automation is named from its entry-point function). Kit automations carry no
	// universally-known prefix, so this protection is best-effort; the exact-name persistence for kit automations at
	// deploy time is a named follow-up (until then a kit-deployed app should not run "Prune Server-Only").
	// DesiredFunctions (from GatherDesiredFunctions) is read only for two non-destructive warnings: a dangling
	// functionName (references a function no effect authors) and a referenced function that is not autonomous-invocable
	// (so the automation cannot run it).
	static void DiffAutomations(
		const TArray<FCrowdyGameModelAutomationInput>& DesiredAutomations,
		const TArray<FCrowdyGameModelAutomationTriggerInput>& DesiredTriggers,
		const TArray<FStudioAutomation>& CurrentAutomations,
		const TArray<FStudioAutomationTrigger>& CurrentTriggers,
		const TArray<FCrowdyGameModelFunctionInput>& DesiredFunctions,
		FCrowdySchemaDelta& InOutDelta,
		const TSet<FString>& RecognizedAutomationNames = TSet<FString>(),
		const TSet<FString>& RecognizedKitTypePrefixes = TSet<FString>(),
		const TSet<FString>& RecognizedKitAutomationNames = TSet<FString>());

	// Augments the desired schema with the SDK-owned Model Collection plumbing: a reserved crowdy_rev int property
	// on every desired container type, and a per-type __crowdy_touch_<type> function that bumps it and emits the
	// model-changed channel notification (so an edge add/remove, a graph mutation the collection nodes make, still
	// produces an observable, notifiable change on the collection's owning container). The touch function names are
	// added to InOutRecognizedFunctionNames so DiffFunctions never prunes them, and its channel notification matches
	// the effect lowering's shape so InjectSessionChannelTarget addresses it and DiffFunctions authors it.
	// A designer attribute that already resolves to crowdy_rev, or a function that already claims the reserved touch
	// name, is a collision: that type's collection plumbing is skipped and OutWarnings names the fix (a plan-time
	// error). Pure (no HTTP/world); the controller calls it right after GatherDesiredFunctions.
	static void AppendReservedCollectionSchema(
		TArray<FCrowdyDesiredContainerType>& InOutTypes,
		TArray<FCrowdyGameModelFunctionInput>& InOutFunctions,
		TSet<FString>& InOutRecognizedFunctionNames,
		TArray<FString>& OutWarnings);

	// True when a notification is one the SDK's effect lowering authors for the model-changed carrier: a channel
	// notification whose payload carries the cmc: prefix, or a spatial notification stamping the model-changed
	// event_type. DiffFunctions REPLACES the SDK's own notification on an upsert while PRESERVING seed/console-
	// authored (non-SDK) ones. Pure + static.
	static bool IsSdkOwnedNotification(const FCrowdyGameModelNotification& Notification);

	// Address every SDK-owned channel notification at the app's default session channel, by NAME: the server injects
	// $session_channel_name per invocation from the app the function is running in, so the destination follows the
	// app that holds the definition. A literal channel id cannot: copied into a recreated or moved app it still
	// resolves, now to a channel nobody in this app is a member of, and the notification is built, signed, broadcast
	// and dropped for want of a recipient with no error anywhere. Any channel_id left by an earlier apply is removed,
	// since the server takes exactly one destination. Called on the desired functions before DiffFunctions.
	static void InjectSessionChannelTarget(TArray<FCrowdyGameModelFunctionInput>& Functions);

	// True when any desired function declares an SDK-owned channel model-changed notification, i.e. the plan depends
	// on the app's session channel existing (naming a channel the app does not have reaches nobody either). The apply
	// path uses this to decide whether a missing session channel is worth auto-creating before it writes. Pure + static.
	static bool AnyFunctionNeedsSessionChannel(const TArray<FCrowdyGameModelFunctionInput>& Functions);
};
