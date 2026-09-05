// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

// Included rather than forward-declared: AttributeOptions defaults its role parameter to a named enumerator, which
// needs the complete type.
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"

class UCrowdyEffect;
class UEdGraphNode;

/**
 * Pure, headless-testable option sources for the effect-graph node authoring pickers. Each returns the candidate
 * strings a node's free-text field offers in its "Pick" dropdown: attribute keys from the effect's container class,
 * the effect's declared tuning magnitudes, the builtin function names, and best-effort authored fn: names. The
 * pickers stay free-text (a value never has to come from the list), so these are suggestions, not a constraint, and
 * they match how the effect asset's own structured picker offers attributes and magnitudes.
 */
/**
 * One builtin the effect language offers. Callee is what the graph emits (the language spells its builtins in
 * lowercase snake_case); DisplayName is the Unreal-style label the palette, the picker, and the node title show.
 * The two are deliberately separate: renaming the label must never change what gets sent to the server.
 */
struct FCrowdyEffectBuiltinCall
{
	FString Callee;
	FString DisplayName;

	// The arity the language accepts. MinArgs == MaxArgs means a fixed-arity builtin, and the node hides its argument
	// count entirely: "not" takes one operand, and offering three would author an expression the server rejects.
	int32 MinArgs = 1;
	int32 MaxArgs = 1;

	FString Description;

	// What each argument is, in order, used to label the node's input pins. A variadic builtin names only its first
	// few; the rest fall back to a numbered label.
	TArray<FString> ArgNames;

	// The palette section this builtin lists under. The grid and permission reads sit in their own section because
	// they behave differently from the pure maths: they hit the database, they are metered, and they are only
	// meaningful in a world that uses grids.
	FString PaletteCategory = TEXT("Functions");

	bool IsFixedArity() const { return MinArgs == MaxArgs; }
};

namespace CrowdyEffectGraphNodeOptions
{
	// The builtin catalog, alphabetical by callee. The DSL does not restrict builtins (the parser passes any
	// identifier through as a call), so this is the authored catalog, not an enforced set.
	const TArray<FCrowdyEffectBuiltinCall>& BuiltinCalls();

	// The catalog entry for a callee, or null when it names no builtin (an authored server function, most often).
	// Case-insensitive.
	const FCrowdyEffectBuiltinCall* FindBuiltin(const FString& Callee);

	// Requested constrained to what the named builtin actually accepts, or returned unchanged when the callee names no
	// builtin (a server function's arity is authored on the server, so the editor cannot know it).
	int32 ClampArgCount(const FString& Callee, int32 Requested);

	// The label for one argument pin: the catalog's name for it, marked "(optional)" past the builtin's minimum
	// arity, or a numbered fallback for a variadic builtin's extra arguments. Empty when the callee names no builtin,
	// so a server call's pins keep their plain names.
	FString ArgDisplayName(const FString& Callee, int32 ArgIndex);

	// The Unreal-style label for a callee ("abs" -> "Abs"), or the callee verbatim when it is not a known builtin, so
	// a hand-typed or authored name still reads as itself. Case-insensitive.
	FString BuiltinDisplayName(const FString& Callee);

	// The Unreal-style label for a bool constant's literal ("true" -> "True"). The stored literal stays lowercase,
	// since that is what the effect language emits; only the label changes. Anything else reads back verbatim.
	FString BoolLiteralDisplayName(const FString& Literal);

	// The UCrowdyEffect that owns the graph a node lives in (the graph is the effect's editor-only subobject, so the
	// node's typed outer is the effect). Null when the node is not parented to an effect.
	const UCrowdyEffect* OwningEffect(const UEdGraphNode* Node);

	// The Server Owned attribute keys of the container Role reads on Effect (the same set the schema sync creates),
	// sorted and de-duplicated. For Target, that is always the effect's container class. For Source, that is the
	// effect's own container class too UNLESS Effect declares a Source Container Type, in which case it is the
	// declared type's class instead: a source of a different container kind reads its own attributes, not the
	// target's. Empty when Effect is null, the relevant class is unset, or a declared Source Container Type does
	// not resolve to a loaded class (offering the target's keys in that case would suggest a name the source does
	// not actually have). Role defaults to Target so an existing call site that has no notion of role keeps its
	// prior behavior unchanged. Used for the Attribute node, the Result writes, and (as a best-effort hint) the
	// Read Ref node, whose referenced container type is not known until runtime and so is never offered a role.
	TArray<FString> AttributeOptions(const UCrowdyEffect* Effect, ECrowdyEffectRole Role = ECrowdyEffectRole::Target);

	// The names of the tuning magnitudes ($params) declared on the effect, in declared order (blank names skipped).
	TArray<FString> MagnitudeOptions(const UCrowdyEffect* Effect);

	// The documented effect-language builtin function names (abs, append, array, at, ceil, clamp, coalesce, ...
	// remove_at, ... to_string) a Call node may invoke. Broader than the text editor's syntax-highlight subset, so the
	// picker offers the full catalog rather than only the highlighted few. The DSL does not enforce this set; it is
	// the authored suggestion list.
	TArray<FString> BuiltinCallOptions();

	// Authored server-function names for a fn: call, sorted and de-duplicated: every OTHER effect on the same
	// container type as Effect that actually authors a return value (a fn: call exists to read one). Filters on the
	// declared function's bAuthorsReturn, not a non-empty declared return type: an effect that returns a bare
	// attribute legitimately declares no type and is still a valid callee. Effect's own effective function name is
	// excluded, since naming itself would author a cycle the server rejects.
	//
	// Deliberately does NOT offer Effect's own named formulas, even though they are also things a fn: call can name
	// on the text and structured surfaces: a formula lives only inside the Structured surface's own expansion, and
	// the graph compiler never copies an effect's formulas into the program it builds, so a graph-authored fn: call
	// naming one would name a function that does not exist on the server. This function backs only the graph Call
	// node's picker, so the catalog is the complete and correct source for it.
	//
	// Returns nothing (never crashes) when Effect is null, has no container class, or no catalog is registered (a
	// cooked build, or the editor module not yet started).
	TArray<FString> FnCallOptions(const UCrowdyEffect* Effect);
}
