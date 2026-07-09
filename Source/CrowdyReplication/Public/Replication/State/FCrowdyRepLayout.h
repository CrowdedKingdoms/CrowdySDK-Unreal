#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UClass;
class FProperty;

/**
 * One CrowdyState-replicated property, resolved live from reflection. This is a runtime-only struct,
 * deliberately NOT a USTRUCT: it holds a raw FProperty*, which has no stable wire form and is never
 * serialized. Only PropertyID travels or bakes; the FProperty* is re-resolved by name against the
 * owning UClass wherever a layout is assembled (live in editor, or from the baked table in a cooked
 * build, Phase 1).
 */
struct FCrowdyRepProperty
{
	// Resolved live from the owning class; never serialized. Re-resolved by name on the baked path.
	const FProperty* Property = nullptr;

	// CityHash64(class path + "::" + name + ":" + canonical type). Stable across builds and machines;
	// identifies the property independent of its position in the layout.
	int64 PropertyID = 0;

	// Delivery scope: delivered only to the owning client, never on the spatial multicast.
	bool bOwnerOnly = false;

	// Excluded from the per-tick Identical diff; pushed only on an explicit dirty-mark.
	bool bManualDirty = false;

	// Opt-in: included in the periodic keyframe heartbeat (re-sent every interval even when unchanged, so a
	// late/desynced observer converges). When false the property still diffs and sends on change every tick;
	// it just is never part of a keyframe. Orthogonal to the diff, so it never affects on-change replication.
	bool bHeartbeat = false;

	// Parameterless notify fired on the receiver after this property is written.
	FName OnRepFunctionName = NAME_None;
};

/**
 * The ordered set of CrowdyState-replicated properties for one class. Declaration order IS the
 * positional wire order: the codec addresses properties by their index here, so the order
 * must be stable across builds. LayoutHash guards exactly that so it folds every property's name and
 * canonical type in order, so adding, removing, reordering, or retyping a replicated property changes
 * the hash and a drifted peer drops cleanly instead of misparsing by position. PropertyID does not
 * include order, so reordering changes only the hash (the positional guard), which is intended.
 *
 * Shadow representation decision (drives Phase 3; recorded here per the Phase 0 plan).
 * The send loop diffs each owned entity's live values against a per-entity "shadow" copy of the last
 * values it sent. Two representations were weighed:
 *   A. A parallel value buffer laid out by this layout's own property slots, one slot per property
 *      initialized with FProperty::InitializeValue. Diffing is a direct FProperty::Identical(live,
 *      shadow) and the post-send update is FProperty::CopyCompleteValue, with zero per-tick decode.
 *      Cost is one InitializeValue/DestroyValue per property per owned entity plus the buffer's
 *      memory (tens of bytes per entity for typical layouts).
 *   B. Keep the shadow as the last keyframe blob and decode it into a scratch container once per tick
 *      before diffing. Lower idle memory, but pays a full decode every tick for every owned entity
 *      exactly the per-tick cost this fast plane exists to avoid.
 * Decision: Approach A (parallel value buffer). At the target scale (100+ owned entities diffed at up
 * to 10 Hz) B's per-tick decode dominates, while A's memory is small and its per-tick work is a
 * bounded set of Identical calls. Phase 3 implements the shadow this way and re-confirms with a
 * benchmark at a representative owned-entity count. Correctness requires only that the shadow holds
 * the last value we sent and we compare against it; the representation is otherwise an internal
 * choice of the replicator.
 */
/**
 * Why a CrowdyState-marked property can or cannot ride the view plane. Discovery logs the precise
 * reason (FCrowdyStateLayoutBuilder::BuildLayout), and later surfaces (the Blueprint compiler
 * extension's variable check, the variable-details dropdown) reuse the same classifier, so every
 * surface reports the identical reason instead of maintaining its own copy.
 */
enum class ECrowdyStatePropertySupport : uint8
{
	Supported,
	Container,        // direct TArray/TSet/TMap out of scope for the whole plan
	NestedContainer,  // a non-net USTRUCT that transitively holds a container unbounded decode alloc
	StaticArray,      // a fixed-size C array (ArrayDim > 1) the positional diff sees only element [0]
	ObjectRef,        // object/soft-object/interface/delegate no stable value-copy wire form here
	Unsupported       // anything else (text, multicast delegate, ...)
};

struct CROWDYREPLICATION_API FCrowdyRepLayout
{
	// The class these properties were resolved from; weak so a layout cache never keeps a class alive.
	TWeakObjectPtr<const UClass> OwnerClass;

	// Declaration order == positional wire order. Includes super-class properties, so a subclass
	// layout is a stable superset of its parent's.
	TArray<FCrowdyRepProperty> Properties;

	// Folds every property's "name:type" in order; guards the positional ordering across builds.
	int64 LayoutHash = 0;

	bool IsValid() const { return OwnerClass.IsValid() && Properties.Num() > 0; }

	// Linear scan; layouts are small (a handful of properties), so a map would not pay off.
	// Returns INDEX_NONE when no property carries InPropertyID.
	int32 IndexOfPropertyID(int64 InPropertyID) const;
};

/**
 * Builds an FCrowdyRepLayout from a class by walking its reflected properties in declaration order
 * (including super-class properties, so a subclass layout is a stable superset) and collecting those
 * marked meta=(CrowdyState). Scope for this plan is POD leaves + USTRUCTs: object references,
 * interfaces, delegates, and containers (TArray/TSet/TMap) are rejected with a clear error and
 * omitted, so a mis-annotated property drops out of the layout rather than corrupting the positional
 * wire order. This is the editor / WITH_METADATA path; cooked builds assemble an equivalent layout
 * from the baked table (Phase 1).
 */
struct CROWDYREPLICATION_API FCrowdyStateLayoutBuilder
{
	// Returns true when Out.IsValid() (a non-empty layout for a live class). A null class, or a class
	// with no accepted CrowdyState properties, yields an empty, invalid layout and returns false.
	static bool BuildLayout(const UClass* Class, FCrowdyRepLayout& Out);

	// Folds the ordered "name:CanonicalParamType" of each property (';'-separated) via
	// FCrowdyTypeIDGenerator::GenerateFromString. The single source of truth for LayoutHash, so a
	// post-filter recompute (after the executor-overlap filter drops a property) is byte-identical to
	// the original build. Returns 0 for an empty list. Skips entries with a null FProperty.
	static int64 ComputeLayoutHash(const TArray<FCrowdyRepProperty>& Properties);

	// The single classifier for whether a CrowdyState-marked property can ride this plane and, if not,
	// why. Scope is single-value POD leaves + non-container-bearing USTRUCTs; object/interface/delegate
	// references, TArray/TSet/TMap containers, static C arrays, and container-bearing structs are
	// rejected. Shared by discovery (BuildLayout) and the editor surfaces that gate on the same rule, so
	// they never disagree. A null property returns Unsupported.
	static ECrowdyStatePropertySupport ClassifyStateProperty(const FProperty* Prop);

	// Convenience over ClassifyStateProperty for the common yes/no gate: true iff
	// ClassifyStateProperty(Prop) == Supported.
	static bool IsStateReplicatable(const FProperty* Prop);

	// The user-facing sentence fragment naming why Support is not Supported (e.g. "is a container ...").
	// BuildLayout composes its log line from this so discovery and any editor surface read identically.
	// Returns an empty string for Supported. The fragment carries neither the property name nor, for the
	// Unsupported case, the canonical type; callers prepend the name and append the type as needed.
	static FString DescribeStateSupport(ECrowdyStatePropertySupport Support);
};
