// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/Effect/CrowdyEffectAst.h"
#include "Replication/GameModel/Effect/CrowdyGameModelAutomationInput.h"
#include "Replication/GameModel/Effect/CrowdyGameModelFunctionInput.h"

/**
 * One declared tuning magnitude a designer exposes on an effect: a named $param they can set without code.
 * Lowering emits these (plus any implicitly-injected param such as source_id) as the function's parameters.
 */
struct FCrowdyEffectParamDecl
{
	// The name as referenced in EffectScript with a leading '$' (stored without the sigil).
	FString Name;

	// "int" | "float" | "bool" | "string" | "container_ref".
	FString ValueType;

	// A JSON-encoded default; empty means the parameter is required.
	FString DefaultValueJson;

	// Author-facing description, carried through to the function parameter.
	FString Description;
};

/**
 * Which realtime carrier a lowered effect's model-driven "model changed" notification uses so peers re-pull.
 * None emits no notification (the acting client may still ping). Channel reaches every member of the app's
 * default session channel regardless of position - the position-independent default, best for turn-based and
 * session-scoped games. Spatial fans out by proximity (the opcode-139 path) for location-bound changes on
 * containers that carry chunk coordinates. The notification names the changed container with the server-injected
 * $self_container_id system param, so nothing needs to be passed in and an automation fan-out names each container.
 */
enum class ECrowdyModelNotificationCarrier : uint8
{
	None,
	Channel,
	Spatial,
};

/**
 * What the project knows about a function a fn:<name>(...) call could reach: enough to judge whether the call can
 * answer with anything, and on what terms. Deliberately declared here rather than on the effect asset: the lowering
 * core is pure and stateless with no UObject dependency, so it must not include the asset header.
 */
struct FCrowdyEffectFnCallee
{
	FString FunctionName;

	// Whether the callee authors a return expression. A fn: call exists to read the callee's value, so a callee
	// that authors no return can never answer one.
	bool bAuthorsReturn = false;

	// The callee's DECLARED return type ("int" | "float" | "bool" | "string"). Legitimately empty when the callee
	// returns a bare attribute and lets the attribute's own type stand, so empty never means "returns nothing";
	// only bAuthorsReturn says that.
	FString ReturnType;

	// The callee's server invokeScope ("player" | "server" | "internal").
	FString InvokeScope;

	// Whether the callee writes any state. A fn: call reads the callee's return value only, so those writes never
	// run, which is worth saying out loud: nothing else about the call reveals it.
	bool bHasMutations = false;
};

/**
 * Everything lowering needs beyond the parsed program: the function's identity, the target container type,
 * and the attribute + magnitude vocabularies it validates references against. self.<attr> always validates
 * against the target container type's attributes. source.<attr> validates against the SOURCE container type's
 * attributes when the effect declares one, and against the target's when it does not, which is the case where the
 * source is another container of the same type.
 */
struct FCrowdyEffectLoweringContext
{
	FString FunctionName;
	FString ContainerTypeName;
	FString Description;

	// The accepted Server Owned attributes of the target container type (from FCrowdyAttributeRegistry).
	TArray<FCrowdyAttributeDef> Attributes;

	// The container type the effect's Source is, when it is a DIFFERENT type from the target. Empty means the
	// source is the same container type as the target, so source.<attr> reads the target's attributes; that is what
	// an effect which declares no source type means, and it is the only behaviour that existed before this field.
	FString SourceContainerTypeName;

	// The accepted Server Owned attributes of that source container type. A declared source type may legitimately
	// have none of its own (a container whose whole contribution is functions declares no attribute), so it is
	// SourceContainerTypeName, and never this list being non-empty, that says a source schema was declared.
	TArray<FCrowdyAttributeDef> SourceAttributes;

	// Set when SourceContainerTypeName names a container type nothing in the project declares: a misspelling, or a
	// container class since deleted or renamed. Lowering reports it as an error instead of checking source.<attr>
	// against the target's attributes, which would accept names the source does not have and fail only once the
	// server ran the function. Resolving a type name to a class needs reflection, so it happens where the context is
	// built; the lowering stays pure and only reports what it is handed.
	bool bSourceContainerTypeUnresolved = false;

	// The designer-declared tuning magnitudes ($params).
	TArray<FCrowdyEffectParamDecl> Magnitudes;

	// The model-changed notification carrier to author (default None, so a raw lowering emits nothing extra;
	// UCrowdyEffect::Compile sets it from the effect's carrier + the project default). When not None, lowering
	// appends the matching notification (naming the container via $self_container_id) to the function input.
	ECrowdyModelNotificationCarrier NotificationCarrier = ECrowdyModelNotificationCarrier::None;

	// Who may invoke the lowered function: "player" (the default), "server" (admins only), or "internal" (reachable
	// only through a fn: call from another function). The server vocabulary verbatim, matching
	// FCrowdyGameModelFunctionInput::InvokeScope; anything else is rejected with a diagnostic.
	FString InvokeScope = TEXT("player");

	// Whether an automation may run the function as the server. Orthogonal to InvokeScope: internal removes the
	// direct player route while this opens the automation one, and a trusted server-side grant sets both.
	bool bAutonomousInvocable = false;

	// The declared return type ("int" | "float" | "bool" | "string"), or empty to leave it to the lowering, which
	// reads it off the attribute when the return is a bare self./source. attribute and otherwise warns.
	FString ReturnType;

	// How this effect's authoring surface writes a return, appended to a diagnostic about a missing one so the
	// message names something the author can actually do. Each surface authors a return differently, and the
	// lowering cannot know which one produced this program. Empty appends nothing.
	FString ReturnAuthoringHint;

	// Resolves the callee of a fn:<name>(...) call against what the project knows about its own container type, so
	// lowering can warn about a call that can never answer with a value. Three states, and the no-false-positive
	// property rests on all three being kept apart:
	//   unset          no catalog is available at all (no editor, a cooked build, a bare unit test). Lowering must
	//                  then emit NO fn: diagnostics whatsoever, since it knows nothing either way.
	//   returns false  the project knows no function by that name on this container type. This is NOT an error: a
	//                  fn: call may legitimately name a hand-authored server function or a kit function that is not
	//                  a Crowdy Effect asset.
	//   returns true   the function is known and OutCallee is filled, so its return can be judged.
	// Function names are compared case-sensitively, matching the server.
	TFunction<bool(const FString& FunctionName, FCrowdyEffectFnCallee& OutCallee)> FnCalleeLookup;
};

// The result of lowering: the function input plus diagnostics. Any Error means the effect is not shippable.
struct FCrowdyEffectLoweringResult
{
	FCrowdyGameModelFunctionInput Function;
	TArray<FCrowdyEffectDiagnostic> Diagnostics;

	// True when the effect reads or writes source.<attr>, so applying it needs a Source object (lowering injects
	// the required source_id container_ref param). The effect asset persists this so the invoke path can enforce
	// "this effect needs a Source" client-side instead of letting the server fail on a missing required param.
	bool bSourceReferenced = false;

	// Set only when the effect opts into running itself (bRunAutomatically): the automation upsert to send alongside
	// the function, and, for a property-change trigger, the event trigger upsert. Both unset for an ordinary effect,
	// so a plain compile is byte-identical to before automations existed. The schema sync (CrowdyStudio) consumes
	// these the same way it consumes Function.
	TOptional<FCrowdyGameModelAutomationInput> Automation;
	TOptional<FCrowdyGameModelAutomationTriggerInput> Trigger;

	bool HasErrors() const
	{
		return Diagnostics.ContainsByPredicate(
			[](const FCrowdyEffectDiagnostic& D) { return D.Severity == ECrowdyEffectSeverity::Error; });
	}
};

/**
 * The shared lowering core: an EffectScript AST plus its context become one gameModelUpsertFunction input.
 * This is the single place effect semantics live, so every front-end (text, structured picker, optional
 * typed-C++) lowers identically. Pure and stateless; no I/O, no UObject.
 *
 * What it does: resolves each self./source. attribute to its server key, applies the compound operator
 * (+=,-=,*=,/=,=) against the current value, wraps the result in the attribute's inherited ClampMin/ClampMax,
 * injects a source_id container_ref param when the effect is cross-entity, infers the default invoke policy
 * (owner_of_self for a pure-self effect, is_participant when a source/ref is involved) unless explicit
 * `require` lines are written, and lowers those requires to the server's invoke-policy boolean tree. It
 * validates attribute names and operator/type fit, warns on undeclared params, and never lets a clamp be
 * written in an effect (that is rejected at parse time). A self. reference is validated against the target
 * container type; a source. reference against the source container type when the context declares one, and against
 * the target's when it does not, which is the source being another container of the same type.
 *
 * It also lowers the program's optional return: the expression is key-resolved exactly like a right-hand side but
 * never clamp-wrapped, since a return reads and writes nothing. Its type is whatever the context declares, or the
 * attribute's own type when the return is a bare self./source. read.
 *
 * It also refuses an effect that cannot mean what it says: one attribute written two different ways on the
 * same self. or source. base (an attribute answers to both its declared name and its lowercased server key,
 * and either is fine on its own, but mixing them for the same base inside one effect reads as two separate
 * values; self and source name attributes on different containers, so each base is checked independently), a
 * literal zero divisor, and a literal too large for the int attribute it is assigned to. Two weaker signals are
 * warnings, since the effect is still valid: a write a later plain assignment replaces before anything reads it
 * (a `require` between them never counts as a read, since every require is evaluated before any mutation runs),
 * and a declared magnitude the effect never mentions. Every diagnostic carries the 1-based line and column it
 * applies to, except one about a magnitude, which has no line of its own.
 */
class CROWDYREPLICATION_API FCrowdyEffectLowering
{
public:
	static FCrowdyEffectLoweringResult Lower(const FCrowdyEffectProgram& Program, const FCrowdyEffectLoweringContext& Context);

	// The $param names a program references that are not in DeclaredNames and are not reserved by the effect layer
	// (source_id and the server-injected system params: self_container_id, caller_user_id, etc). De-duplicated, in
	// first-seen order. Pure
	// and front-end-agnostic (a program comes from the text parser or the structured builder), so the picker's
	// "Add missing magnitudes" action and its test share one definition of "undeclared".
	static TArray<FString> CollectUndeclaredParams(const FCrowdyEffectProgram& Program, const TArray<FString>& DeclaredNames);

	// The $param names the effect layer owns: source_id is the container_ref this lowering injects for the `source`
	// sugar, and the rest are values the server injects into a condition. Nothing an author declares may claim one,
	// and a read of one never warns as undeclared. Exposed because a tuning magnitude is not the only thing that can
	// collide with these: a timer parameter is validated against the same list, and two copies of it would drift.
	static bool IsReservedParamName(const FString& Name);
};
