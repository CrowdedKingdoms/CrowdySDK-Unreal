// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectNotificationCarrier.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"
#include "Replication/GameModel/Effect/CrowdyGameModelAutomationInput.h"
#include "CrowdyEffect.generated.h"

class FAssetRegistryTagsContext;
class FDataValidationContext;
class UCurveFloat;
class UEdGraph;

/**
 * The value type of a tuning magnitude, authoritative in the effect asset. Each entry maps to a Game Model
 * wire type ("int", "float", "bool", "string", "container_ref") the server function schema uses; the mapping
 * is UCrowdyEffect::ValueTypeToWireString / WireStringToValueType.
 */
UENUM(BlueprintType)
enum class ECrowdyEffectValueType : uint8
{
	Int,
	Float,
	Bool,
	String,
	ContainerRef
};

/**
 * The type of value an effect answers with when it is invoked. None means the effect only changes state, or that
 * the type is read off the returned attribute (a bare Target/Source attribute return types itself). The concrete
 * entries mirror ECrowdyEffectValueType minus ContainerRef, which no shipped Game Model function returns and the
 * server schema does not constrain, so it is left out rather than shipped unverified.
 */
UENUM(BlueprintType)
enum class ECrowdyEffectReturnType : uint8
{
	None UMETA(DisplayName = "None (or read from the returned attribute)"),
	Int,

	// Every Game Model function shipped so far returns an int, a string or a bool, so a float return has not been
	// exercised against a live server. It is offered because the server declares the field as a free string with no
	// stated vocabulary, but confirm a float upsert is accepted before relying on it.
	Float,
	Bool,
	String
};

/**
 * Who is trusted to invoke an effect's function. One control rather than several overlapping flags: it decides the
 * server's invokeScope, and it is independent of whether an automation may run the effect (an effect can be closed
 * to players and still be an automation's entry point).
 */
UENUM(BlueprintType)
enum class ECrowdyEffectCallableFrom : uint8
{
	// Any player the invoke policy admits. The default, and what every effect authored before this control did.
	Players UMETA(DisplayName = "Players"),

	// Admin/server callers only. No player route at all.
	ServerOnly UMETA(DisplayName = "Server only"),

	// Reachable only through a fn: call from another effect's expression: a shared formula with no entry point of
	// its own. Such an effect has to return a value, since a fn: call is what reads it.
	OtherEffectsOnly UMETA(DisplayName = "Other effects only")
};

/**
 * When an effect opts into running itself, how it is triggered. A single flat choice the author picks; each maps
 * to the server's trigger wire shape. EveryInterval and Cron are schedule triggers (triggerType "schedule",
 * scheduleKind "interval" / "cron"); the rest are event triggers (triggerType "event").
 *
 * OnPropertyChange (onEvent "property_changed") fires when a watched property changes. Which writes it observes
 * depends on the write source: by default both a direct property write and a write a Model function makes as part
 * of its own execution. OnFunctionInvoked (onEvent "function_invoked") fires when another Model function commits,
 * which is the direct way to have one effect react to another running.
 *
 * OnPlayerLeft (onEvent "player_left") fires once per actor the platform stops seeing, the last player's included,
 * so it is the one trigger that runs for an app that has just emptied. OnPlayerCountChanged (onEvent
 * "player_count_changed") fires on a transition of the app's active-player gauge, coalesced on the trailing edge,
 * and never at zero. Neither takes a filter; the function reads the event through its own parameters
 * (user_id, remaining_player_count, ... for player_left; the server documents each event's list).
 */
UENUM(BlueprintType)
enum class ECrowdyEffectAutomationTrigger : uint8
{
	EveryInterval UMETA(DisplayName = "Every N milliseconds"),
	Cron UMETA(DisplayName = "Cron schedule"),
	OnPropertyChange UMETA(DisplayName = "On property change"),
	OnFunctionInvoked UMETA(DisplayName = "On function invoked"),
	OnPlayerLeft UMETA(DisplayName = "On player left"),
	OnPlayerCountChanged UMETA(DisplayName = "On player count changed")
};

/**
 * Which writes a property-change trigger observes. A property can change two ways: a direct gameModelSetProperty
 * call made outside any function, or a mutation applied inside an invoke, an automation run, or a timer fire. Most
 * game logic writes properties from inside functions, so a trigger that observes only direct writes never sees the
 * change it was authored for. Maps to the server trigger's writeSource; only a property-change trigger accepts it,
 * since the server rejects a filter the event cannot match.
 */
UENUM(BlueprintType)
enum class ECrowdyEffectPropertyWriteSource : uint8
{
	Any UMETA(DisplayName = "Any write"),
	Direct UMETA(DisplayName = "Direct writes only"),
	Function UMETA(DisplayName = "Writes from inside a function")
};

/**
 * Which containers a running automation fans out over. Type (the default) runs the function once per container of
 * the effect's own type; Container targets one specific container by id; Global runs once app-wide. Maps to the
 * server targetMode "type" / "container" / "global".
 */
UENUM(BlueprintType)
enum class ECrowdyEffectAutomationTargetMode : uint8
{
	Container,
	Type,
	Global
};

/**
 * The plain (non-reflected) snapshot of an effect's Automation authoring fields, so the authoring-to-wire mapping
 * (UCrowdyEffect::BuildAutomationInput) is pure and unit-testable with no UObject. UCrowdyEffect fills this from
 * its own Automation UPROPERTYs before compiling; tests build it directly. Field defaults mirror the effect's
 * UPROPERTY defaults and the FCrowdyGameModelAutomationInput budget defaults.
 */
struct FCrowdyEffectAutomationAuthoring
{
	bool bRunAutomatically = false;
	bool bEnabled = true;

	ECrowdyEffectAutomationTrigger Trigger = ECrowdyEffectAutomationTrigger::EveryInterval;
	int32 IntervalMs = 1000;
	FString CronExpr;

	FString ChangePropertyKey;
	ECrowdyEffectPropertyWriteSource WriteSource = ECrowdyEffectPropertyWriteSource::Any;
	FString WatchFunctionName;
	FString ChangeContainerType;
	int32 DebounceMs = 0;

	ECrowdyEffectAutomationTargetMode TargetMode = ECrowdyEffectAutomationTargetMode::Type;
	FString TargetTypeOverride;
	FString TargetContainerId;

	FString AutomationName;

	int32 MaxTargets = 50;
	int32 GasLimit = 100000;
	int32 RunTimeoutMs = 2000;
	int32 MaxRunsPerMinute = 120;
	int32 FailureThreshold = 5;
	int32 CooldownMs = 30000;
};

/**
 * A designer-tunable parameter on an effect: a named value exposed in the effect asset so it can be tuned without
 * touching the effect body. Lowers to one of the function's parameters.
 *
 * The type is named "Magnitude" for serialization compatibility and reads as "Tuning Parameter" everywhere it is
 * shown, matching the graph's Tuning Parameter node. Do not rename the C++ symbol: it would break existing assets
 * and Blueprint graphs for a label change.
 */
USTRUCT(BlueprintType, meta = (DisplayName = "Tuning Parameter"))
struct FCrowdyEffectMagnitude
{
	GENERATED_BODY()

	// The parameter name, referenced in EffectScript as "$Name" (stored without the sigil), e.g. "base_power".
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Name;

	// The authoritative value type. The typed default editor and the lowering both read this; the legacy
	// ValueType string below is kept only as a serialization mirror for assets authored before the enum.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	ECrowdyEffectValueType ValueTypeEnum = ECrowdyEffectValueType::Int;

	// Legacy wire-string value type, retained for back-compat with assets serialized before ValueTypeEnum
	// existed. Folded into ValueTypeEnum on load (UCrowdyEffect::MigrateMagnitude) and mirrored from it on edit,
	// so it always matches the enum. No longer shown in the panel. Do not rename: it is a serialization key.
	UPROPERTY()
	FString ValueType = TEXT("int");

	// Whether a caller MUST supply this parameter. Independent of the default value: this alone decides required-ness,
	// so a parameter can be required whatever its type, and an optional one may default to an empty string.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	bool bRequired = false;

	// A JSON-encoded default value ("5", "\"text\"", "true"), used when a caller supplies none. Ignored while the
	// parameter is Required: a required parameter is lowered and applied with no default at all, and the value is
	// kept only so unticking Required restores what was authored.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString DefaultValueJson;

	// An optional curve that supplies this magnitude's value, sampled at apply time by the Level input on Apply
	// (for example, damage that scales with level). When set it is sampled in place of DefaultValueJson; an explicit
	// Override passed to Apply still wins. Numeric magnitudes only (int or float; an int rounds the sampled value). A
	// curve always yields a value, so a curve-bound magnitude never has to be supplied by a caller even when it is
	// marked Required.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	TObjectPtr<UCurveFloat> Curve = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Description;

	// Set once the legacy ValueType string has been folded into ValueTypeEnum, so a later reload does not
	// re-derive (and clobber) an enum the designer has since changed. Defaults false so an asset serialized
	// before this field migrates on first load.
	UPROPERTY()
	bool bTypeMigrated = false;

	// Set once bRequired has been derived from the older encoding, where an empty default was what made a parameter
	// required, so a later reload does not re-derive (and clobber) a flag the designer has since changed. Defaults
	// false so an asset serialized before bRequired existed migrates on first load.
	//
	// Saving this asset from a build that predates these two fields drops both, and re-loading it here then derives
	// required-ness from the default text again. "Required with a default" has no legacy spelling, so that one
	// combination cannot survive the round trip and comes back optional.
	UPROPERTY()
	bool bRequiredMigrated = false;

	// This magnitude's name as an FName, for looking it up in a caller's Overrides map. Find-only: a name nothing has
	// ever interned cannot be a key in that map either, so resolving it to None is the correct answer and the name
	// table is not grown by a parameter no caller overrides. Derived from Name on every call rather than cached, so
	// there is no second copy of the name to fall out of step with the one that is serialized.
	FName GetNameKey() const { return FName(*Name, FNAME_Find); }
};

/**
 * One signal this effect fires: a message to clients that something happened, carrying no state change of its own.
 * Use it when a client should just react (play an effect, start a sequence, run some Blueprint) and there is no
 * property worth replicating to stand in for the message.
 *
 * It lowers to a model-driven notification on the effect's function, so it rides the same realtime carrier the SDK
 * already uses and needs no extra plumbing. An effect may fire signals and change nothing else, which is the pure
 * "signal only" case the server supports by leaving a function's mutations empty.
 *
 * On arrival each bound container gets a PARAMETERLESS function called on it, named by prefixing the signal:
 * "BossWave" calls "OnSignal_BossWave". That mirrors the CrowdyOnRep convention, and like CrowdyOnRep the handler
 * takes no arguments and reads whatever it needs off the container.
 */
USTRUCT(BlueprintType, meta = (DisplayName = "Signal"))
struct FCrowdyEffectSignal
{
	GENERATED_BODY()

	// The signal's name. It travels on the wire and picks the handler, so it has to be a valid function-name
	// suffix: letters, digits and underscores, not starting with a digit. Validation rejects anything else rather
	// than letting it fail silently at dispatch.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Description;
};

// One value baked into a timer's delayed invocation, evaluated when the timer ARMS rather than when it fires. This
// is the per-attempt snapshot a delayed completion needs: the value is captured now, so a dedupe replacement armed
// by a later attempt cannot be validated against a value that has since moved on.
USTRUCT(BlueprintType)
struct FCrowdyEffectTimerParam
{
	GENERATED_BODY()

	// The parameter's name, referenced on the target function as $<name>. A name the effect layer already owns
	// (source_id and the other server-injected names) is rejected at compile time.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Name;

	// A model expression, evaluated when the timer arms rather than when it fires. Passed through verbatim, exactly
	// like Delay Expression and Dedupe Key Expression above; a literal string needs its own quotes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString Expression;
};

/**
 * One delayed invocation this effect arms when it runs: "after this effect commits, run FunctionName again in N
 * milliseconds". The server arms it in the same transaction as the effect's writes, so a failed effect schedules
 * nothing and a successful one is guaranteed to fire even if the API restarts. A deadline that passes while the
 * app has nobody in it waits and fires when a player returns: late, not lost.
 *
 * The delay and the dedupe key reach the server as model EXPRESSIONS, not literals, which is a sharp edge: a bare
 * word there parses as an identifier and the upsert is rejected. This struct is shaped so an author cannot hit
 * that. A plain millisecond count and a plain key are the normal fields, and the lowering emits each in valid
 * expression form (digits for the delay, a quoted and escaped string for the key). The two advanced override
 * fields exist for the case where the value genuinely has to be computed from model state.
 */
USTRUCT(BlueprintType, meta = (DisplayName = "Timer"))
struct FCrowdyEffectTimer
{
	GENERATED_BODY()

	// The Model function to run when the timer fires. It must be autonomous-invocable, since a timer fires headlessly
	// with no player in the request. Naming this effect's own function is legal and reschedules it; the server bounds
	// that chain by its cascade depth rather than letting it repeat forever.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString FunctionName;

	// How long to wait before firing, in milliseconds. Ignored when Delay Expression is set.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect",
		meta = (DisplayName = "Delay (ms)", ClampMin = "1"))
	int32 DelayMs = 1000;

	// An app-scoped key that makes re-arming REPLACE the pending timer instead of queueing another fire. This is how
	// a countdown gets reset, and how a hot effect avoids flooding the timer queue. Written as plain text; the
	// lowering quotes and escapes it. Leave empty to queue an independent fire each time.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect")
	FString DedupeKey;

	// The container the delayed call runs against. Empty means "self", the container this effect ran on.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect",
		meta = (DisplayName = "Target (optional)"))
	FString Target;

	// Advanced: a model expression supplying the delay instead of the fixed Delay (ms), for a wait that depends on
	// model state (for example self.respawn_delay_ms). Must be expression source, so a literal string needs quotes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect",
		meta = (DisplayName = "Delay Expression (advanced)"))
	FString DelayExpression;

	// Advanced: a model expression supplying the dedupe key instead of the plain Dedupe Key above, for a key that
	// varies per container. Must be expression source; unlike Dedupe Key it is NOT quoted for you.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect",
		meta = (DisplayName = "Dedupe Key Expression (advanced)"))
	FString DedupeKeyExpression;

	// Values to bind into the delayed invocation as $<name> parameters, each evaluated when this timer arms. This is
	// how a delayed completion gets a per-attempt snapshot to validate against, instead of reading whatever the
	// target container's state happens to be when the timer finally fires.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowdy Effect", meta = (DisplayName = "Timer Parameters"))
	TArray<FCrowdyEffectTimerParam> Params;
};

/**
 * The no-cycle seam that lets UCrowdyEffect (this runtime module) ask whether another effect asset in the
 * project already authors the same function on the same container type, without CrowdyReplication depending on
 * the asset registry or any editor module. CrowdySDKEditor registers the hook at startup, backed by an
 * asset-registry-driven index it keeps warm rather than re-sweeping the project on every call, so validating many
 * effects in one pass (a validate-on-save or cook-time sweep) stays cheap. Mirrors the CrowdyEffectGraphCompileHook
 * seam: a runtime module holds the entry point, an editor module supplies the implementation. Editor-only in
 * practice: IsDataValid itself is WITH_EDITOR-gated, and no hook is registered outside the editor (e.g. a
 * cooked build), in which case FindOtherEffectsWithFunctionName simply reports nothing found.
 *
 * The identity is the (container type, function name) PAIR, matching how the server scopes a model function: the
 * same function name on two different container types is two distinct server functions and not a conflict.
 */
namespace CrowdyEffectDuplicateFunctionCheck
{
	// One other effect asset that currently authors the same function name on the same container type as the
	// asset being checked.
	struct FConflict
	{
		FString AssetPath;
		FString ContainerTypeName;
		FString FunctionName;
	};

	using FSweepHook = TFunction<TArray<FConflict>(
		const FString& AssetPath, const FString& ContainerTypeName, const FString& FunctionName)>;

	// Registered by CrowdySDKEditor at module startup, cleared to null at shutdown.
	CROWDYREPLICATION_API void SetSweepHook(FSweepHook Hook);

	// Every OTHER Crowdy Effect asset (identified by path, excluding AssetPath itself) that currently authors
	// FunctionName on ContainerTypeName. Without a registered hook, returns an empty array, so IsDataValid never
	// reports a false conflict when the editor module has not loaded.
	CROWDYREPLICATION_API TArray<FConflict> FindOtherEffectsWithFunctionName(
		const FString& AssetPath, const FString& ContainerTypeName, const FString& FunctionName);
}

/**
 * The two things a fn: caller needs to know about an effect it might name, read off the authoring surface alone.
 * They answer different questions and neither implies the other: a query effect returns a value and does nothing
 * else, an ordinary effect acts and returns nothing, and both shapes are perfectly valid to author.
 */
struct FCrowdyEffectAuthoredShape
{
	// Whether the effect authors a return expression, and so can answer a fn: call at all.
	bool bAuthorsReturn = false;

	// Whether the effect has any side effect at all: a state write, a signal it fires, or a timer it arms. Despite
	// the name this is broader than writes, because a fn: call runs none of the three.
	bool bHasMutations = false;
};

/**
 * The no-cycle seam that lets this runtime module ask which Game Model functions the project's Crowdy Effect assets
 * declare on a given container type, without CrowdyReplication depending on the asset registry or any editor module.
 * CrowdySDKEditor registers the hook at startup, backed by an asset-registry-driven index it keeps warm rather than
 * re-sweeping the project on every call, so the callers that ask repeatedly (a graph picker filling its list of
 * callable effects, a live script diagnostic that re-runs as the author types) stay cheap. Mirrors the
 * CrowdyEffectDuplicateFunctionCheck seam: a runtime module holds the entry point, an editor module supplies the
 * implementation. Editor-only in practice, since no hook is registered outside the editor (e.g. a cooked build).
 *
 * A function's identity is the (container type, function name) PAIR, matching how the server scopes a model
 * function: the same function name on two different container types is two distinct server functions.
 */
namespace CrowdyEffectFunctionCatalog
{
	// One Game Model function that a Crowdy Effect asset in this project declares.
	struct FDeclaredFunction
	{
		FString FunctionName;
		FString ContainerTypeName;

		// The author's DECLARED return type ("int" | "float" | "bool" | "string"). Legitimately EMPTY when the
		// effect returns a bare attribute and lets the attribute's own type stand, so an empty value here says
		// nothing about whether the effect returns at all.
		FString ReturnType;

		// The server invokeScope ("player" | "server" | "internal").
		FString InvokeScope;

		// Whether the effect actually authors a return expression. This, and NOT a non-empty ReturnType, is what
		// decides whether the function can be named by a fn: call, since a fn: call exists to read a value.
		bool bAuthorsReturn = false;

		// Whether the effect has any side effect at all: a state write, a signal it fires, or a timer it arms.
		// Despite the name this is broader than writes, because a fn: call reads the callee's return value and
		// nothing else, so none of the three run: an author who reaches for one expecting it to change state,
		// notify, or schedule gets silence.
		bool bHasMutations = false;

		FString AssetPath;
	};

	using FHook = TFunction<TArray<FDeclaredFunction>(const FString& ContainerTypeName)>;

	// Registered by CrowdySDKEditor at module startup, cleared to null at shutdown.
	CROWDYREPLICATION_API void SetCatalogHook(FHook Hook);

	// Whether a catalog is registered at all. Branch on this rather than on an empty result: "there is no catalog"
	// and "the catalog knows of no such function" are different answers, and only the second is grounds for a
	// diagnostic. Confusing the two would report every fn: call as unknown the moment the editor module is absent.
	CROWDYREPLICATION_API bool IsCatalogAvailable();

	// Every function the project's Crowdy Effect assets declare on ContainerTypeName. Without a registered hook,
	// returns an empty array.
	CROWDYREPLICATION_API TArray<FDeclaredFunction> FindFunctionsOnContainerType(const FString& ContainerTypeName);
}

/**
 * Whether Compile consults the project's fn-callee catalog when lowering fn: calls. The catalog contributes
 * WARNING diagnostics only (a call to a known function that returns nothing, a call whose writes will not run);
 * the lowered function and every Error diagnostic are identical either way. Building the catalog on a cache miss
 * synchronously loads every Crowdy Effect asset in the project, so a caller that discards warnings (a schema-sync
 * plan, a Blueprint node expansion, a bRequiresSource refresh) passes None and never pays for answers it throws
 * away; a caller that shows the author diagnostics keeps Project.
 */
enum class ECrowdyEffectFnCatalog : uint8
{
	Project,
	None,
};

/**
 * The authored artifact for one Game Model effect: a target container class, an EffectScript body, and the
 * tuning magnitudes it exposes. Compiling it (parse + lower) yields the neutral FCrowdyGameModelFunctionInput
 * a later schema sync sends to gameModelUpsertFunction. This is the diff-friendly, power-user authoring path;
 * the node graph (Source::Graph) edits the same asset and lowers through the same core.
 *
 * Editor validation runs through the engine's IsDataValid hook, so a malformed effect fails asset validation
 * and, when validate-on-cook is enabled, the cook. Compilation needs the target class's discovered attributes
 * (FCrowdyAttributeRegistry), which read live UPROPERTY metadata in the editor and the baked attribute table
 * in a cooked build, so Compile works in both.
 */
UCLASS(BlueprintType)
class CROWDYREPLICATION_API UCrowdyEffect : public UDataAsset
{
	GENERATED_BODY()

public:
	// The container class this effect targets. Must carry a CrowdyContainer tag and Server Owned attributes.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect")
	TSoftClassPtr<UObject> ContainerClass;

	// The server function name to author. Empty falls back to the asset's own name.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect")
	FString FunctionName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect")
	FString Description;

	// Display-only: the word shown for the Source role wherever an authoring surface names it in a sentence (for
	// example "Attacker" instead of "Source"). Purely cosmetic - it never affects Compile(), the lowering, or the
	// wire output; the Source role still lowers to the AST's source regardless of what this reads.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect")
	FString SourceRoleLabel = TEXT("Source");

	// The container type the Source is, for an effect whose source is a different kind of container from its target
	// (a chest an adventurer opens, a trap that damages whoever stepped on it). source.<attr> is then checked against
	// THAT type's attributes. Leave it empty when the source is another container of the target's own type, which is
	// what an effect that does not set it means, and what every effect authored before this field means.
	//
	// It holds the server's container type name, so it is picked from the types the project declares rather than
	// typed: a name nothing declares is refused when the effect compiles, since checking the source's reads against
	// the target's attributes would accept names the source does not have.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect",
		meta = (DisplayName = "Source Container Type"))
	FString SourceContainerType;

	// Which authoring surface this effect compiles from: the EffectScript text, or the node graph. Both lower to
	// the identical function input, so switching is a UI choice, not a semantic one.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect")
	ECrowdyEffectSource Source = ECrowdyEffectSource::Text;

	// The EffectScript body (one or more assignment / require lines). See CrowdyEffectAst for the surface.
	// Compiled only when Source is Text.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect",
		meta = (MultiLine = true, EditCondition = "Source == ECrowdyEffectSource::Text", EditConditionHides))
	FString EffectScript;

#if WITH_EDITORONLY_DATA
	// The node graph authoring this effect, when Source is Graph. Editor-only, exactly like a Material's
	// UMaterialGraph: it owns the editor node objects, is never cooked, and is edited through the graph editor
	// (not the Details panel). Compiling reads it through the editor graph compiler, registered as a no-cycle hook
	// since that compiler lives in the editor module. Held by the base UEdGraph type so this runtime header does not
	// depend on the editor node classes.
	UPROPERTY()
	TObjectPtr<UEdGraph> EffectGraph;
#endif

	// The tuning parameters this effect exposes, so it can be balanced without editing the effect body.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect", meta = (DisplayName = "Tuning Parameters"))
	TArray<FCrowdyEffectMagnitude> Magnitudes;

	// Signals this effect fires when it runs. An effect may declare signals and change nothing else, in which case
	// it is a pure notification with no state change.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect", meta = (DisplayName = "Signals"))
	TArray<FCrowdyEffectSignal> Signals;

	// The server's per-function ceiling on declared timers. Checked at compile time so the limit surfaces while
	// authoring rather than as a rejected upsert during a sync.
	static constexpr int32 MaxTimersPerEffect = 4;

	// Delayed invocations this effect arms when it commits. Each is armed transactionally with the effect's writes,
	// so nothing is scheduled if the effect fails. At most MaxTimersPerEffect of them.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect", meta = (DisplayName = "Timers"))
	TArray<FCrowdyEffectTimer> Timers;

	// The type of value this effect answers with. Leave it None for an effect that only changes state, or for one
	// whose return is a single attribute (the attribute's own type is used). Declaring a type is what lets a caller
	// receive the value already decoded instead of as raw JSON.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect")
	ECrowdyEffectReturnType ReturnType = ECrowdyEffectReturnType::None;

	// Who may invoke this effect. Other effects only makes it a shared formula with no entry point of its own,
	// which requires a return value for a fn: call to read.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect")
	ECrowdyEffectCallableFrom CallableFrom = ECrowdyEffectCallableFrom::Players;

	// Which realtime carrier this effect's model-changed notification uses so peers re-pull. Default defers to the
	// project's DefaultModelNotificationCarrier (which itself defaults to Channel). Compile() resolves this into the
	// lowering's carrier, which authors the notification naming the changed container via the server-injected
	// $self_container_id, so nothing is passed in and an automation fan-out names each container correctly.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect")
	ECrowdyEffectNotificationCarrier NotificationCarrier = ECrowdyEffectNotificationCarrier::Default;

	// Whether repeated applies of this effect may be merged into ONE server call. OFF by default, so an effect that
	// does not ask for it is sent exactly once per apply, exactly as it always has been.
	//
	// Turn it on for an effect a player applies many times a second (autofire damage, a channelled drain). The server
	// admits only a limited number of Game Model calls per player per app, shared across every Game Model call the
	// game makes, and an uncoalesced autofire spends that allowance in seconds; past it, calls are refused.
	//
	// Two applies merge only when they agree on everything except the accumulated parameter: the same target
	// container, the same function, the same session, and the same value for every other tuning parameter. An apply
	// that differs in any of those is sent on its own, so one attacker's hit is never credited to another's.
	//
	// This is only correct when the effect's body ACCUMULATES the parameter rather than assigning from it:
	// "self.hp = self.hp - $damage" sums correctly across a window, "self.hp = 100 - $damage" does not.
	//
	// Merged applies also SHARE one return value, since there is only one call: each caller sees the state after the
	// whole window, not after its own contribution. An effect whose return value has to be read per apply should not
	// be coalesced.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect|Coalescing",
		meta = (DisplayName = "Coalesce Repeated Applies"))
	bool bCoalescable = false;

	// How long a merge window stays open, in seconds, measured from the FIRST apply that opened it. A later apply
	// joins an open window but never pushes its deadline back, so sustained fire still lands on a fixed cadence
	// instead of being held forever. Keep it short: it is added latency on every apply that opens a window.
	//
	// The runtime may hold a window open LONGER than this while the invoke allowance is running low. It never
	// shortens it, so this is a floor on the merge interval, not a promise about when the call is sent.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect|Coalescing",
		meta = (DisplayName = "Coalesce Window (seconds)", ClampMin = "0.01", ClampMax = "2.0", UIMin = "0.02",
			UIMax = "1.0", EditCondition = "bCoalescable", EditConditionHides))
	float CoalesceWindowSeconds = 0.1f;

	// The tuning parameter whose values are SUMMED across a merge window: ten applies of 5 damage become one call
	// carrying 50. It must name a tuning parameter of this effect typed int or float. A name that matches nothing, or
	// matches a parameter of any other type, cannot be summed: the effect is then applied uncoalesced (correct, just
	// as expensive as before) rather than sent with a wrong value.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Crowdy Effect|Coalescing",
		meta = (DisplayName = "Accumulate Parameter", EditCondition = "bCoalescable", EditConditionHides))
	FString AccumulateParam;

	// Read-only: whether this effect reads or writes source.<attr> and so must be applied with a Source object.
	// Recomputed from the lowering whenever the effect is edited (PostEditChangeProperty) and read by the invoke
	// path to reject a missing Source client-side instead of letting the server fail on the missing source_id param.
	// An effect not re-saved since this field was introduced reads false, which just skips the guardrail (no false
	// rejection); re-save it to refresh.
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Crowdy Effect")
	bool bRequiresSource = false;

	// When set, this effect also runs itself server-side as an autonomous process (an NPC tick, a spawner, a world
	// job): the compile marks its function autonomous-invocable and emits an automation. Everything else in the
	// Automation section only applies while this is on.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation")
	bool bRunAutomatically = false;

	// Whether the automation is eligible to run once deployed. Turn off to author it but keep it paused.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (EditCondition = "bRunAutomatically", EditConditionHides))
	bool bEnabled = true;

	// Legacy "players cannot invoke" flag, replaced by the Callable From control. Retained as a serialization key so
	// an asset authored before that control loads with the same invoke scope (UCrowdyEffect::MigrateCallableFrom),
	// and no longer shown in the panel. Do not rename.
	UPROPERTY()
	bool bAutonomousOnly = false;

	// Set once bAutonomousOnly has been folded into CallableFrom, so a later load does not stomp a scope the author
	// has since changed. Defaults false so an asset serialized before CallableFrom migrates on first load.
	UPROPERTY()
	bool bInvokeScopeMigrated = false;

	// How the automation fires: on a fixed interval, on a cron schedule, when a watched property changes, or when
	// another Model function is invoked.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (EditCondition = "bRunAutomatically", EditConditionHides))
	ECrowdyEffectAutomationTrigger AutomationTrigger = ECrowdyEffectAutomationTrigger::EveryInterval;

	// The run period in milliseconds when the trigger is Every N milliseconds.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (DisplayName = "Interval (ms)", ClampMin = "1",
			EditCondition = "bRunAutomatically && AutomationTrigger == ECrowdyEffectAutomationTrigger::EveryInterval",
			EditConditionHides))
	int32 AutomationIntervalMs = 1000;

	// The cron expression when the trigger is Cron schedule.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (DisplayName = "Cron Expression",
			EditCondition = "bRunAutomatically && AutomationTrigger == ECrowdyEffectAutomationTrigger::Cron",
			EditConditionHides))
	FString AutomationCronExpr;

	// The property key that fires the automation when it changes, for the On property change trigger. Only a direct
	// property write fires this - a property a Model function mutates as part of its own execution does not.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (DisplayName = "On Property Key",
			EditCondition = "bRunAutomatically && AutomationTrigger == ECrowdyEffectAutomationTrigger::OnPropertyChange",
			EditConditionHides))
	FString AutomationChangePropertyKey;

	// Which writes count as a change, for the On property change trigger. Any (the default) observes both a direct
	// property write and one a Model function makes while it runs. Narrow it to Direct only when a function's own
	// mutations should deliberately not re-trigger the automation.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (DisplayName = "Observe Writes From",
			EditCondition = "bRunAutomatically && AutomationTrigger == ECrowdyEffectAutomationTrigger::OnPropertyChange",
			EditConditionHides))
	ECrowdyEffectPropertyWriteSource AutomationWriteSource = ECrowdyEffectPropertyWriteSource::Any;

	// The Model function whose invocation fires the automation, for the On function invoked trigger. This is the
	// function being WATCHED, not this effect's own function.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (DisplayName = "Watch Function Name",
			EditCondition = "bRunAutomatically && AutomationTrigger == ECrowdyEffectAutomationTrigger::OnFunctionInvoked",
			EditConditionHides))
	FString AutomationWatchFunctionName;

	// An optional container type filter on the watched event. Its meaning follows the trigger, and so does what an
	// empty value means.
	//
	// For On property change it is the type whose property changed, and empty watches this effect's own container
	// type, since an effect normally reacts to its own state.
	//
	// For On function invoked it is the type the WATCHED function runs on, which is usually a different type from
	// this effect's (one container type reacting to something happening on another). Empty therefore matches every
	// type rather than defaulting to this effect's own, because defaulting there would author "fires when the
	// watched function runs on my own type" and silently never match the common cross-type case.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (DisplayName = "On Container Type (optional)",
			EditCondition = "bRunAutomatically && (AutomationTrigger == ECrowdyEffectAutomationTrigger::OnPropertyChange || AutomationTrigger == ECrowdyEffectAutomationTrigger::OnFunctionInvoked)",
			EditConditionHides))
	FString AutomationChangeContainerType;

	// A coalesce window in milliseconds for an event trigger: the first fire in the window wins and the rest are
	// dropped. 0 fires on every event. A player-left trigger never coalesces: each leave is its own run.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (DisplayName = "Debounce (ms)", ClampMin = "0",
			EditCondition = "bRunAutomatically && AutomationTrigger != ECrowdyEffectAutomationTrigger::EveryInterval && AutomationTrigger != ECrowdyEffectAutomationTrigger::Cron",
			EditConditionHides))
	int32 AutomationDebounceMs = 0;

	// Which containers the automation runs over. Defaults to every container of the effect's own type.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (EditCondition = "bRunAutomatically", EditConditionHides))
	ECrowdyEffectAutomationTargetMode AutomationTargetMode = ECrowdyEffectAutomationTargetMode::Type;

	// An optional container type to fan out over instead of the effect's own type, for the Type target mode.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (DisplayName = "Target Type Override (optional)",
			EditCondition = "bRunAutomatically && AutomationTargetMode == ECrowdyEffectAutomationTargetMode::Type",
			EditConditionHides))
	FString AutomationTargetTypeOverride;

	// The container the automation runs on. Both Container and Global need one: a global run is a single run
	// against this container, not a run against nothing. It is an id rather than a name, so it belongs to the app
	// it was read from and an asset carrying one does not survive being copied to another app.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (DisplayName = "Target Container Id",
			EditCondition = "bRunAutomatically && (AutomationTargetMode == ECrowdyEffectAutomationTargetMode::Container || AutomationTargetMode == ECrowdyEffectAutomationTargetMode::Global)",
			EditConditionHides))
	FString AutomationTargetContainerId;

	// The automation name (unique per app). Empty defaults to the effective function name.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation",
		meta = (DisplayName = "Automation Name (optional)",
			EditCondition = "bRunAutomatically", EditConditionHides))
	FString AutomationName;

	// The safety budget bounding a runaway automation. Sensible defaults, collapsed out of the way; most authors
	// never touch these.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation|Safety",
		meta = (DisplayName = "Max Targets", ClampMin = "1", EditCondition = "bRunAutomatically", EditConditionHides))
	int32 AutomationMaxTargets = 50;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation|Safety",
	// The Game API clamps this to a platform ceiling, so a higher figure is authored and then silently not used.
	// The default is the ceiling the platform ships with; raise it only against a tier known to allow more.
		meta = (DisplayName = "Gas Limit", ClampMin = "1", EditCondition = "bRunAutomatically", EditConditionHides))
	int32 AutomationGasLimit = 20000;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation|Safety",
		meta = (DisplayName = "Run Timeout (ms)", ClampMin = "1", EditCondition = "bRunAutomatically", EditConditionHides))
	int32 AutomationRunTimeoutMs = 200;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation|Safety",
		meta = (DisplayName = "Max Runs Per Minute", ClampMin = "1", EditCondition = "bRunAutomatically", EditConditionHides))
	int32 AutomationMaxRunsPerMinute = 120;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation|Safety",
		meta = (DisplayName = "Failure Threshold", ClampMin = "1", EditCondition = "bRunAutomatically", EditConditionHides))
	int32 AutomationFailureThreshold = 5;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Automation|Safety",
		meta = (DisplayName = "Cooldown (ms)", ClampMin = "0", EditCondition = "bRunAutomatically", EditConditionHides))
	int32 AutomationCooldownMs = 30000;

	// The effective function name (FunctionName, or the asset name when that is empty).
	FString GetEffectiveFunctionName() const;

	// The server container type this effect's function lives on: the CrowdyContainer tag of the target class.
	// Empty when no container class is set or the class carries no tag (an effect that cannot compile). Resolving
	// it loads the target class, so treat it as the same cost as Compile().
	FString GetContainerTypeName() const;

	// The class the declared Source Container Type names, or null when no source type is declared or no loaded class
	// declares that name. One resolution path shared by the compile and the authoring surfaces, so the attributes an
	// author is offered for the source are the same ones the compile checks against. It scans the loaded classes
	// (FCrowdyAttributeRegistry::FindContainerClassByTypeName), so treat it as the same cost as Compile().
	UClass* ResolveSourceContainerClass() const;

	// Snapshot this effect's Automation UPROPERTYs into the plain authoring struct BuildAutomationInput consumes.
	FCrowdyEffectAutomationAuthoring GetAutomationAuthoring() const;

	// Pure authoring-to-wire mapping, shared by Compile and its tests with no UObject. Produces the automation
	// upsert input for the given entry-point function and its container type; when the trigger is a property change,
	// fills OutTrigger with the matching event trigger (otherwise leaves it unset). The automation name defaults to
	// the function name, the type target defaults to the container type, and the budget is always emitted so a
	// re-sync read-back matches.
	//
	// The automation's model-driven notification names its container via the server-injected $self_container_id,
	// evaluated per run, so no params are baked here: a Container-mode run and a Type-mode fan-out both name the
	// right container from the same authored notification. ParamsJson stays empty (the server default "{}").
	static FCrowdyGameModelAutomationInput BuildAutomationInput(const FCrowdyEffectAutomationAuthoring& Authoring,
		const FString& FunctionName, const FString& ContainerTypeName,
		TOptional<FCrowdyGameModelAutomationTriggerInput>& OutTrigger);

	// Whether a signal name is usable: non-empty, and legal as the tail of a function name (letters, digits and
	// underscores, not starting with a digit). Enforced at author time because an unusable name fails at dispatch,
	// on a remote machine, with nothing to point at.
	static bool IsValidSignalName(const FString& Name);

	// The channel notification one signal lowers to: payload = concat("csg:<name>:", $self_container_id). The
	// container id is appended by the server per invocation, so one authored signal names the right container for a
	// player invoke and for every container of an automation fan-out alike. Pure and static so the payload shape is
	// unit-tested against the decoder without an asset.
	static FCrowdyGameModelNotification BuildSignalNotification(const FString& SignalName);

	// The parameterless handler a signal calls: "BossWave" gives "OnSignal_BossWave". Shared by the authoring side
	// (so validation and tooling report the exact name to implement) and the receive side (so dispatch derives the
	// same name), which is what stops the two drifting apart.
	static FName MakeSignalHandlerName(const FString& SignalName);

	// Map one authored timer onto its wire form, resolving each of the two authoring routes into expression source:
	// the plain Delay (ms) becomes its own digits unless Delay Expression overrides it, and the plain Dedupe Key
	// becomes a quoted, escaped string literal unless Dedupe Key Expression overrides it. Each Params row is carried
	// across as a name/expression pair verbatim, and a row with an empty name is dropped rather than emitted
	// nameless. Pure and static so the quoting rule, which is the easiest thing to get wrong here, is unit-tested
	// without an asset.
	static FCrowdyGameModelTimer BuildTimerInput(const FCrowdyEffectTimer& Authored);

	// The Game Model wire type ("int"|"float"|"bool"|"string"|"container_ref") for a value type, and the
	// reverse (Int on an unrecognized string; case- and whitespace-tolerant). Pure, so the wire string a
	// lowering / schema sync sees is identical whether it started from the enum or a legacy string.
	static FString ValueTypeToWireString(ECrowdyEffectValueType ValueType);
	static ECrowdyEffectValueType WireStringToValueType(const FString& Wire);

	// The server invokeScope ("player"|"server"|"internal") for a Callable From choice, and the server returnType
	// ("" for None, else "int"|"float"|"bool"|"string"). Pure, so the authored value and a read-back compare on
	// identical ground.
	static FString CallableFromToWireString(ECrowdyEffectCallableFrom CallableFrom);
	static FString ReturnTypeToWireString(ECrowdyEffectReturnType ReturnType);

	// The Callable From an asset should load with, folding the legacy autonomous-only flag. It maps to Server only
	// exactly when the legacy pair said so, which is only when the effect also ran automatically: bAutonomousOnly
	// was hidden and inert otherwise, so widening it now would silently close an effect players can invoke today.
	// Pure + static so the mapping is unit-tested; PostLoad applies it once, guarded by bInvokeScopeMigrated.
	static ECrowdyEffectCallableFrom MigrateCallableFrom(ECrowdyEffectCallableFrom Current,
		bool bLegacyAutonomousOnly, bool bLegacyRunAutomatically);

	// The server trigger writeSource ("any"|"direct"|"function") for a write source, and the reverse (Any on an
	// unrecognized string; case- and whitespace-tolerant) for reading a trigger back off the server. Pure, so the
	// authored value and the read-back value compare on identical ground and a re-sync does not churn.
	static FString WriteSourceToWireString(ECrowdyEffectPropertyWriteSource WriteSource);
	static ECrowdyEffectPropertyWriteSource WireStringToWriteSource(const FString& Wire);

	// Load-time normalization of one magnitude: trim the name, fold the legacy ValueType string into
	// ValueTypeEnum once (guarded by bTypeMigrated), mirror the string back from the enum so the two never
	// disagree, resolve bRequired once (guarded by bRequiredMigrated, and keeping an explicitly set flag rather than
	// deriving over it), and give an optional bool the explicit false its checkbox cannot otherwise express.
	// Idempotent. Static + public so migration is unit-tested directly.
	static void MigrateMagnitude(FCrowdyEffectMagnitude& Magnitude);

	// Whether Name matches a tuning parameter typed int or float, filling bOutInteger for the int case. This is the
	// one test for "can this parameter be summed", shared by asset validation and by the apply path, so an effect that
	// validates as coalescable is exactly the set that actually coalesces at runtime. The name is matched
	// case-sensitively, because a tuning parameter name is a wire key. Pure + static so it is table-testable.
	static bool IsNumericMagnitude(const TArray<FCrowdyEffectMagnitude>& Magnitudes, const FString& Name,
		bool& bOutInteger);

	// This effect's coalescing answer, resolved once and cached on the asset: whether it opts into merging at all,
	// and if so the trimmed name of the parameter to sum and whether that parameter is an int. False for an effect
	// that does not opt in, or whose Accumulate Parameter names nothing summable.
	//
	// It is the same decision IsNumericMagnitude makes, but an apply path that runs many times a second must not
	// re-scan the magnitude list and re-trim the same two strings on every single apply. Cached lazily, invalidated
	// on load and on every edit. Game-thread only, like every other apply-path read.
	bool TryGetCoalesceSpec(FString& OutAccumulateParam, bool& bOutInteger) const;

	// Drops the cached coalescing answer, so the next apply resolves it again. Called on load and on edit; also
	// public so a test that mutates an effect's coalescing fields directly (no editor, no PostLoad) can do the same.
	void InvalidateCoalesceSpec() const;

	// The value type to marshal / lower a magnitude by: the typed enum once the magnitude has been migrated (an
	// asset that has been loaded or edited in the editor), else the legacy wire string. This keeps a magnitude
	// built directly in code (which sets only ValueType and never runs through load/edit) typed as authored.
	static ECrowdyEffectValueType ResolveMagnitudeValueType(const FCrowdyEffectMagnitude& Magnitude);

	// Whether a caller must supply this magnitude: the bRequired flag once the magnitude has been migrated OR has
	// been set to true outright, else the older encoding it is derived from. An explicit true is honoured without
	// the migration flag so that code and Blueprint setting only bRequired are not silently ignored; an explicit
	// false cannot be, because it is indistinguishable from the field's own default value.
	static bool ResolveMagnitudeRequired(const FCrowdyEffectMagnitude& Magnitude);

	// Required-ness as the encoding that predates bRequired expressed it: an empty default meant "the caller must
	// supply this", except on a bool, whose checkbox cannot draw an empty default and so never meant it. The ONE
	// definition of that rule; the asset path and the stored-payload path both read it, so a value type spelled
	// with stray whitespace cannot be a bool under one and not the other.
	static bool IsLegacyRequiredEncoding(ECrowdyEffectValueType ValueType, const FString& DefaultValueJson);

	// Gives an optional bool the explicit false its checkbox cannot otherwise express, leaving every other value
	// type and every required magnitude untouched. Reads the resolved value type, so an unmigrated magnitude that
	// names bool only in its legacy string is normalized too.
	static void EnsureBoolDefault(FCrowdyEffectMagnitude& Magnitude);

	// The required-ness and default a magnitude takes when the designer changes its value type, which discards the
	// old default because it was canonical for the old type. A bool becomes optional with an explicit false, since a
	// checkbox cannot express "no value"; every other type is left with no default and so becomes required. Never
	// produces optional-with-no-default, which is the one shape the wire cannot describe.
	static void ApplyValueTypeChangeDefaults(ECrowdyEffectValueType NewType, bool& bOutRequired,
		FString& OutDefaultValueJson);

	// Resolve this effect's NotificationCarrier against the project default into the pure lowering carrier Compile()
	// feeds the lowering. Reads UCrowdySDKDeveloperSettings; safe on the game thread (CDO-backed settings).
	ECrowdyModelNotificationCarrier ResolveNotificationCarrier() const;

	// The pure resolution rule, split out so it is table-testable with no settings object: an effect-level Default
	// defers to the project carrier; a project-level Default (or any unrecognized value) falls back to Channel, the
	// SDK's position-independent default. A concrete effect-level value always wins.
	static ECrowdyModelNotificationCarrier ResolveCarrier(ECrowdyEffectNotificationCarrier EffectCarrier,
		ECrowdyEffectNotificationCarrier ProjectCarrier);

	// What this effect's selected authoring surface declares, as far as a fn: caller is concerned. Answered from the
	// very program Compile() would lower, so the two can never disagree about whether a return really exists: a
	// return the author switched on but left unfilled does not build, and is reported here as no return at all.
	// Still cheap, because building that program needs neither the container class nor the attribute vocabulary,
	// and the editor's project-wide function index calls this once per effect asset in a single sweep. Both answers
	// come from one program rather than a query each. A Graph-sourced effect goes through the editor graph
	// compiler, so both answers are false in a build where that is unavailable.
	FCrowdyEffectAuthoredShape GetAuthoredShape() const;

	// Parse + lower this effect against its container class's attributes. The result carries the lowered
	// function input and every parse/lowering diagnostic; a parse error short-circuits before lowering. When
	// the container class cannot be resolved or carries no CrowdyContainer tag, the result holds one Error.
	// FnCatalog picks whether the fn-callee catalog is consulted; see ECrowdyEffectFnCatalog for what that
	// changes (advisory warnings only) and what it can cost (a project-wide asset load).
	FCrowdyEffectLoweringResult Compile(ECrowdyEffectFnCatalog FnCatalog = ECrowdyEffectFnCatalog::Project) const;

	// The lowering diagnostics an EffectScript body would produce against this effect's settings, for a body the
	// asset has not committed yet. This is what an editor needs to underline a problem as it is typed: the parser
	// alone only knows syntax, so a misspelled attribute, a mixed spelling, or an operator a type cannot take are
	// all invisible to it. Returns nothing when the body does not parse (the caller already reports that) or when
	// there is no container type to check against.
	TArray<FCrowdyEffectDiagnostic> DiagnoseScriptBody(const FString& Body) const;

	// Migrates each magnitude's legacy value-type string into the typed enum, derives its required flag from the
	// older empty-default encoding, and trims magnitude names, so an asset authored before either field loads and
	// behaves identically.
	virtual void PostLoad() override;

#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& ValidationContext) const override;

	// Recomputes bRequiresSource from the lowering on every edit.
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	// Stamps what this effect authors onto its own asset data when the package is saved, so a tool that needs to
	// know what the effect declares can read it off the asset registry instead of loading the package. Only a save
	// writes it, and an effect whose authoring surface does not currently build writes nothing at all.
	virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
#endif

private:
	// The cached answer behind TryGetCoalesceSpec. Kind is -1 until it has been resolved, 0 when nothing about this
	// effect is summable, 1 for an int parameter and 2 for a float one. Not serialized: it is derived entirely from
	// fields that are.
	mutable int8 CachedCoalesceKind = -1;
	mutable FString CachedAccumulateParam;
};
