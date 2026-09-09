// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h"
#include "Replication/GameModel/Effect/CrowdyEffectSpec.h"

/**
 * The tag keys a Crowdy Effect asset stamps on its own FAssetData when it saves, so a schema plan can read what the
 * effect authors without loading the package. Both are written together or not at all.
 */
namespace CrowdyEffectTagKeys
{
	// The authored surface, serialized. Never written empty: the asset registry treats a zero-length tag value as a
	// programming error rather than as "nothing to say".
	inline const TCHAR* AuthoredSurface = TEXT("CrowdyEffectSurface");

	// The payload's format version, so a reader can tell a payload it understands from one an older or newer plugin
	// wrote. A payload whose version is not the exact current one is ignored and its asset is loaded instead.
	inline const TCHAR* AuthoredSurfaceVersion = TEXT("CrowdyEffectSurfaceVer");
}

/**
 * Everything a Crowdy Effect's authoring surface says, resolved and detached from the asset.
 *
 * This is the ONE shape a compile reads. A compile of the live asset snapshots it first, and a compile of a stored
 * payload parses it, so the two paths cannot answer differently about what the effect authors: they are the same
 * code reading the same struct.
 *
 * Everything here is stored RESOLVED, because several of the asset's own fields do not mean what they say on their
 * own:
 *   - the function name falls back to the asset's name when the author left it blank, so the fallback is applied
 *     here rather than left to a reader that has no asset to fall back to;
 *   - a magnitude's value type and the invoke scope each have a legacy field and a migration flag behind them, so
 *     the wire form is resolved here and the legacy fields never travel;
 *   - the surface that produced the program is recorded, because a text body and a graph body that happen to read
 *     the same are not the same effect and must not describe each other.
 *
 * What is deliberately NOT here: the "needs a Source" flag. That is an OUTPUT of lowering, recomputed from the
 * program every compile. Reading it back in as an input would let a stale stored value decide what the compile
 * concludes.
 *
 * The project's default notification carrier is likewise absent. It is a project setting a plan reads at plan time,
 * not something the asset authored, so only the effect's own (possibly deferring) choice is stored.
 */
struct FCrowdyEffectAuthoredSurface
{
	// The format version of the serialized form, and the only thing that makes a stored payload readable at all.
	// Bump it for any of three changes, each of which silently re-meanings payloads already on disk:
	//   - a field added, removed, or given a different meaning;
	//   - any enum this payload stores by integer value reordered, since a stored value is read as the enum that
	//     existed when it was written;
	//   - a change to what the node-graph compiler produces, since a graph effect's payload carries the compiler's
	//     OUTPUT rather than the graph, and nothing else would notice that the two no longer agree.
	//
	// Version 2 normalizes an optional bool's default to "false" when the key is absent, which version 1 did not, so
	// a version 1 payload describes a bool parameter the asset itself would describe differently.
	static constexpr int32 TagVersion = 2;

	// The name the function is authored under, with the blank-means-the-asset-name fallback already applied.
	FString EffectiveFunctionName;

	FString Description;

	// The target container class, as a soft class path. Which container TYPE that is stays a plan-time lookup: the
	// class can gain, lose or change its type tag without the effect being touched, so baking the type name here
	// would be recording someone else's answer.
	FString ContainerClassPath;

	// The declared source container type for a cross-type effect, empty when the source shares the target's type.
	FString SourceContainerType;

	// Which authoring surface produced the program below. Without it a text effect and a graph effect with
	// equivalent bodies are indistinguishable, and each would describe the other.
	ECrowdyEffectSource Source = ECrowdyEffectSource::Text;

	// The EffectScript body, meaningful only when Source is Text.
	FString ScriptText;

	// The graph compiled to its spec, meaningful only when Source is Graph. The graph itself is editor-only node
	// objects; the spec is the compiler's own output and is what the lowering consumes.
	FCrowdyEffectSpec GraphSpec;

	// The tuning parameters, each already carrying its resolved wire value type.
	TArray<FCrowdyEffectParamDecl> Magnitudes;

	TArray<FCrowdyEffectSignal> Signals;
	TArray<FCrowdyEffectTimer> Timers;

	// The declared return type in wire form ("int" | "float" | "bool" | "string"), empty for an effect that answers
	// with nothing or lets the returned attribute's own type stand.
	FString ReturnType;

	// The resolved invoke scope in wire form ("player" | "server" | "internal"), with the legacy autonomous-only
	// flag already folded in.
	FString InvokeScope;

	// The effect's OWN carrier choice, unresolved. Default means it defers to the project setting, which is a
	// plan-time input rather than part of what the asset authored.
	ECrowdyEffectNotificationCarrier NotificationCarrier = ECrowdyEffectNotificationCarrier::Default;

	FCrowdyEffectAutomationAuthoring Automation;
};

/**
 * The attribute vocabularies one compile validates against: the target container type's attributes, and the source
 * type's when the effect declares a different one.
 *
 * Passed IN rather than discovered inside the compile. Discovering them needs a live UClass for the target and a
 * scan of every loaded class for the source, which quietly makes "is this effect valid" depend on which packages
 * happen to be in memory: a source type whose class is not resident reads as a type nothing declares, and every
 * cross-type effect in the project fails at once. As a parameter, a caller that already knows the project's
 * container types can answer with nothing loaded at all.
 */
struct FCrowdyEffectVocabulary
{
	FString TargetTypeName;
	TArray<FCrowdyAttributeDef> TargetAttributes;

	// Empty when the effect declares no source type, which means the source is another container of the target's
	// own type.
	FString SourceTypeName;
	TArray<FCrowdyAttributeDef> SourceAttributes;

	// Set when a declared source type name matches no container type the caller knows of. Kept apart from an empty
	// attribute list: a real container type may legitimately declare no attributes of its own.
	bool bSourceTypeUnresolved = false;
};

/** One container type's attribute vocabulary, named rather than reflected, as a caller supplies it to a compile. */
struct FCrowdyEffectVocabularyType
{
	FString TypeName;
	TArray<FCrowdyAttributeDef> Attributes;
};

namespace CrowdyEffectAuthoredSurface
{
	/**
	 * Snapshot a live effect asset. A Graph-sourced effect is compiled to its spec here, and any diagnostic that
	 * raises is reported through OutDiagnostics rather than swallowed: a surface whose graph did not compile
	 * describes nothing, and every caller has to decide what to do about that.
	 */
	CROWDYREPLICATION_API FCrowdyEffectAuthoredSurface FromEffect(
		const UCrowdyEffect& Effect, TArray<FCrowdyEffectDiagnostic>& OutDiagnostics);

	/**
	 * Everything except the graph's compiled spec, for a caller that brings its own program: a live diagnostic of a
	 * body the author is still typing checks that text, not the one the asset holds, so compiling the graph would be
	 * work thrown away on every keystroke.
	 *
	 * NOT compilable on its own. A Graph-sourced effect comes back with an empty spec, which would lower to a
	 * function that does nothing rather than to what the effect authors. Use it only to build a lowering context.
	 */
	CROWDYREPLICATION_API FCrowdyEffectAuthoredSurface FromEffectSettings(const UCrowdyEffect& Effect);

	// The serialized form of a surface. Deterministic: every field is written in a fixed order and every list keeps
	// its authored order, so the same surface always produces byte-identical text.
	CROWDYREPLICATION_API FString ToJson(const FCrowdyEffectAuthoredSurface& Surface);

	// Whether a stored version tag names the exact format this build reads. Anything else, including text that is
	// not a number at all, means "not mine": a payload written by an older or newer plugin is passed over in favour
	// of the asset itself rather than read as though its fields still meant the same thing.
	CROWDYREPLICATION_API bool IsCurrentTagVersion(const FString& VersionText);

	// The reverse. False when the text is not a surface this build understands, which the caller treats as "no
	// payload" rather than as an empty effect.
	CROWDYREPLICATION_API bool FromJson(const FString& Json, FCrowdyEffectAuthoredSurface& OutSurface);

	/**
	 * The payload to stamp on the asset, or false when there is nothing true to write (a graph that does not
	 * compile).
	 *
	 * A failure writes NOTHING. A reader that finds no payload loads the asset, which is the correct answer and
	 * repairs itself the moment the asset saves cleanly again. Recording the failure instead would make it
	 * permanent, and the graph compiler is only present in a process that loaded the editor tooling, so a bulk
	 * resave anywhere else would otherwise stamp "does not compile" onto every graph effect at once.
	 */
	CROWDYREPLICATION_API bool BuildTagPayload(const UCrowdyEffect& Effect, FString& OutPayload);

	// The target container class the surface names, resolved from its stored path. Null when the path is empty or
	// names nothing loadable. Resolving a container class is not the same as loading the effect asset: the container
	// classes a schema plan touches are resident by the time it reads effects, so this is a lookup in practice.
	CROWDYREPLICATION_API UClass* ResolveTargetClass(const FCrowdyEffectAuthoredSurface& Surface);

	// The carrier a compile of this surface authors its model-changed notification on: the effect's own choice, or
	// the project default when it defers to one. Reads the developer settings, which is why it is separate from the
	// pure compile below.
	CROWDYREPLICATION_API ECrowdyModelNotificationCarrier ResolveCarrier(const FCrowdyEffectAuthoredSurface& Surface);

	// The vocabularies as reflection answers them: the target class's attributes, and, for a declared source type,
	// the attributes of whichever loaded class carries that type name. This is the reflection-backed provider; a
	// caller that already holds the project's container types uses FromTypes instead and loads nothing.
	CROWDYREPLICATION_API FCrowdyEffectVocabulary ResolveVocabularyFromClasses(
		const UClass* TargetClass, const FString& TargetTypeName, const FString& DeclaredSourceType);

	// The same answer from a caller-supplied list of container types. No reflection, no class lookup, so it holds
	// with nothing loaded. Type names are matched case-sensitively, exactly as the server keys them.
	CROWDYREPLICATION_API FCrowdyEffectVocabulary ResolveVocabularyFromTypes(
		const TArray<FCrowdyEffectVocabularyType>& Types, const FString& TargetTypeName,
		const FString& DeclaredSourceType);

	// The lowering context a surface plus its vocabularies describe. Pure: no class, no asset registry, no scan of
	// loaded objects. FnCatalog decides whether the context carries a fn: callee lookup at all.
	CROWDYREPLICATION_API FCrowdyEffectLoweringContext BuildLoweringContext(
		const FCrowdyEffectAuthoredSurface& Surface, const FCrowdyEffectVocabulary& Vocabulary,
		ECrowdyModelNotificationCarrier Carrier, ECrowdyEffectFnCatalog FnCatalog);

	/**
	 * Build the program from whichever surface authored it, lower it, and append the declarative parts (signals,
	 * timers, the automation) exactly as a compile of the live asset does. This IS the compile: UCrowdyEffect
	 * routes through it, so there is only one implementation to keep correct.
	 *
	 * PriorDiagnostics carries anything the caller already knows about the surface (the graph compile's own
	 * diagnostics), so they are reported in the same place and in the same order a single-pass compile reported
	 * them, and an error among them short-circuits before lowering.
	 */
	CROWDYREPLICATION_API FCrowdyEffectLoweringResult Compile(
		const FCrowdyEffectAuthoredSurface& Surface, const FCrowdyEffectVocabulary& Vocabulary,
		ECrowdyModelNotificationCarrier Carrier, ECrowdyEffectFnCatalog FnCatalog,
		const TArray<FCrowdyEffectDiagnostic>& PriorDiagnostics);

	/**
	 * The whole compile from a surface alone: resolve its target class, read that class's container type and
	 * attributes, resolve the carrier against the project setting, and compile. UCrowdyEffect::Compile is this
	 * function applied to a freshly taken snapshot, so a caller holding a stored surface gets the identical result,
	 * including for an effect whose target class is missing or untagged.
	 *
	 * OutTargetTypeName receives the container type the effect's function lives on, empty when it could not be
	 * resolved, which is the same answer the asset itself gives for that state.
	 */
	CROWDYREPLICATION_API FCrowdyEffectLoweringResult CompileResolved(
		const FCrowdyEffectAuthoredSurface& Surface, ECrowdyEffectFnCatalog FnCatalog,
		const TArray<FCrowdyEffectDiagnostic>& PriorDiagnostics, FString& OutTargetTypeName);
}
