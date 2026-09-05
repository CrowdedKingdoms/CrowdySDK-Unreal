// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameModel/CrowdySchemaSync.h" // FCrowdyDesiredContainerType, FCrowdyEffectPlanRecord
#include "UObject/SoftObjectPath.h"

class UCrowdyEffect;
struct FCrowdyEffectAuthoredSurface;
struct FCrowdyEffectDiagnostic;

/**
 * One Crowdy Effect asset as a plan sees it BEFORE anything is loaded: where it lives, the content key its stored
 * compile result would have to match, whether that stored result may be reused, and what the asset itself said it
 * authors the last time it was saved. Produced by FCrowdyEffectPlanCache::ProbeEffectAssets from the asset registry
 * alone, so probing a project costs no package loads at all.
 */
struct FCrowdyEffectAssetProbe
{
	FSoftObjectPath ObjectPath;
	FString AssetPath; // the object path string, which is also the cache key

	// The asset registry's saved hash for the asset's package. Empty when the registry has none (a package that has
	// never been saved), which is treated as "unknown" and always recompiles.
	FString PackageHash;

	// The vocabulary key of the container types the cached record compiled against (its target type, and its declared
	// source type when it has one), as THIS plan reflected them. Empty when the cache holds nothing for this asset.
	FString VocabularyHash;

	// The asset's package is resident with unsaved edits, which makes the saved hash stale by definition.
	bool bDirty = false;

	// Every part of the key matched, so the stored compile result stands and this asset needs neither streaming
	// nor compiling.
	bool bReuseCached = false;

	// The authored surface the asset stamped on itself when it was last saved, when the registry carries one at the
	// version this build understands and the package is not resident with unsaved edits. Non-empty means the record
	// can be built without the asset ever being loaded.
	FString SurfacePayload;
	bool bHasSurfaceTag = false;

	// Whether the asset has to be in memory before records are built. False when a stored compile result stands, and
	// false when the registry already carries the asset's authored surface, since the record comes straight from
	// that. A caller that streams assets in ahead of BuildRecords streams exactly these.
	bool NeedsLoad() const { return !bReuseCached && !bHasSurfaceTag; }
};

/**
 * The state one plan's effect sweep shares between its two gathers: the container vocabulary the plan reflected
 * (half of the cache key), the cross-plan compile cache, the probe the caller streamed from, and the compile
 * records themselves. Passing the same context to both gathers is what stops the second one re-sweeping the
 * project, and passing a cache is what stops an unchanged effect being loaded again on a later plan.
 */
struct FCrowdyEffectGatherContext
{
	// The desired container types as reflected from code, BEFORE any SDK-owned augmentation: an effect compiles
	// against the designer's attributes, so the reserved collection plumbing must not move this key.
	TArray<FCrowdyDesiredContainerType> DesiredTypes;

	// Null disables reuse entirely: every asset is loaded and compiled, and nothing is stored.
	FCrowdyEffectPlanCache* Cache = nullptr;

	TArray<FCrowdyEffectAssetProbe> Probes;
	bool bProbed = false;

	TArray<FCrowdyEffectPlanRecord> Records;
	bool bRecordsBuilt = false;
};

/**
 * A per-editor-session store of what each Crowdy Effect asset last compiled to, so the second and later schema-sync
 * plans load only the effects that actually changed instead of force-loading every one of them.
 *
 * The key is a pair, and BOTH halves have to match for a stored result to stand:
 *
 *  1. The asset registry's package saved hash, read without loading anything. It covers every serialized field of
 *     the asset, which is what makes it usable at all: an effect's authoring surface includes an editor-only node
 *     graph, and no hand-written field-by-field hash could ever see that.
 *  2. A vocabulary key over every container type the compile validates against as the current plan reflected it: the
 *     effect's target type, and the source type it declares for a cross-type write. For each, the type name and each
 *     attribute's key, value type, visibility, writability, default and clamp bounds, in a canonical order. An
 *     effect's body compiles against those vocabularies (an assignment even inherits the attribute's clamp bounds), so
 *     a container class gaining, losing or retyping an attribute must invalidate every effect that reads it, even
 *     though none of those effect assets changed. Both types matter for the same reason: the attribute could be
 *     renamed on either one, and neither rename touches the effect asset itself.
 *
 * On top of the pair there are two unconditional rules. A package that is resident and dirty is neither reused nor
 * stored: unsaved edits are invisible to the saved hash, so such a compile can only be filed under a key that
 * describes different content, and reverting or reloading the asset would then serve it as a valid hit. And a record
 * that could not resolve a container type, or that failed to compile, is never stored either: whatever it was waiting
 * on lives outside both halves of the key, so reusing it would keep an effect broken after the thing it was missing
 * arrived.
 *
 * A project setting the compile reads (the default model notification carrier) is not part of any single asset's
 * key; it is a whole-cache salt, so changing it drops every stored record at the start of the next plan.
 *
 * The store holds plain data only: no UObject pointer ever survives a frame here.
 */
class FCrowdyEffectPlanCache
{
public:
	// The editor-session-wide instance. Held as a function-local static rather than a member of anything, since the
	// Studio window (and its controller) is created and destroyed freely while the cache should outlive it.
	static FCrowdyEffectPlanCache& Get();

	// Drops the instance's contents. Called from the module's shutdown so nothing survives a module reload.
	static void Shutdown();

	// The canonical text a container type's attribute vocabulary reduces to, and its hash. Pure: the answer comes
	// only from DesiredTypes, so no reflection happens here. Props are emitted in a case-sensitive sorted order, so
	// a reflection pass that returns the same attributes in a different order produces the same key. A type name
	// absent from DesiredTypes produces a distinct "absent" text rather than an empty one, so "the type is gone" and
	// "the type has no attributes" are never the same key.
	static FString MakeVocabularyText(const FString& TypeName, const TArray<FCrowdyDesiredContainerType>& DesiredTypes);
	static FString ComputeVocabularyHash(const FString& TypeName, const TArray<FCrowdyDesiredContainerType>& DesiredTypes);

	// The same key over BOTH vocabularies a compile reads: the target type, and the declared source type of a
	// cross-type write. An empty source type name hashes identically to the target-only form, so an effect that
	// declares no source keys exactly as it did before source types existed. Covering the source type is what makes
	// the key complete: an attribute renamed on the SOURCE container moves neither the effect's package hash nor the
	// target's vocabulary, so without it a stored compile referencing the old name reads as a valid hit and the sync
	// pushes a function naming an attribute that no longer exists.
	static FString ComputeVocabularyHash(
		const FString& TargetTypeName, const FString& SourceTypeName,
		const TArray<FCrowdyDesiredContainerType>& DesiredTypes);

	// Whether a stored record may be reused against a freshly probed key. Pure + static, so every rule above is
	// testable without an asset: a dirty package always misses, an unknown hash on either side always misses, and
	// both halves of the key must match exactly (case-sensitively -- these are hex digests, and FString's default
	// comparison is case-insensitive).
	static bool IsRecordReusable(
		const FCrowdyEffectPlanRecord& Record, const FString& PackageHash, const FString& VocabularyHash,
		bool bPackageDirty);

	// Whether a freshly compiled record is worth storing at all. A record whose compile failed, or whose target
	// container type did not resolve, is waiting on something neither half of the key can see (a class that has not
	// been created yet, a tag that has not been added), so it is always recompiled rather than remembered.
	//
	// bPackageDirty is the asset's state at the moment it was compiled, and it is required rather than defaulted
	// because getting it wrong is silent: a compile of unsaved edits carries the SAVED package's hash, so storing it
	// files content that is in no asset in the project under the key of the content that IS. The registry's hash does
	// not move when the package is reverted or reloaded, so that record then reads as a valid hit and the sync ships
	// edits the designer threw away.
	static bool ShouldStoreRecord(const FCrowdyEffectPlanRecord& Record, bool bPackageDirty);

	// The whole-cache salt: the project settings a compile reads but no asset's own key covers. Not pure (it reads
	// the developer settings CDO), which is why it is separate from BeginPlan.
	static FString ComputeGlobalSalt();

	// Starts a plan against a salt. A salt that differs from the one the stored records were compiled under empties
	// the cache, since none of them can be trusted any more.
	void BeginPlan(const FString& InSalt);

	const FCrowdyEffectPlanRecord* Find(const FString& AssetPath) const;
	void Store(const FCrowdyEffectPlanRecord& Record);
	void Clear();
	int32 Num() const { return Records.Num(); }

	// Enumerate every Crowdy Effect asset in the project through the asset registry and key each one, loading
	// nothing. The package hashes are read in ONE batched registry call rather than one call per asset, and each
	// asset's saved authored surface is read off the same query.
	static void ProbeEffectAssets(FCrowdyEffectGatherContext& Context);

	/**
	 * One compile record per effect asset, in three descending rungs:
	 *
	 *  1. a stored compile result whose whole key still matches is taken verbatim;
	 *  2. otherwise, when the registry carries the asset's saved authored surface, the record is built from that
	 *     and the package is never touched;
	 *  3. otherwise the asset is resolved (already resident when the caller streamed it in) or loaded, and
	 *     compiled, exactly as it always was.
	 *
	 * A package that is resident with unsaved edits never reaches rung 2, because the saved surface describes
	 * content the designer has since changed. Idempotent per context, so both gathers can call it and only the
	 * first does any work.
	 */
	static const TArray<FCrowdyEffectPlanRecord>& BuildRecords(FCrowdyEffectGatherContext& Context);

private:
	// Compile one resident effect asset into its record. Never loads: the caller has already resolved the object.
	static FCrowdyEffectPlanRecord CompileRecord(
		const UCrowdyEffect& Effect, const FCrowdyEffectAssetProbe& Probe,
		const TArray<FCrowdyDesiredContainerType>& DesiredTypes);

	// The one implementation of "what does this effect compile to", shared by the loaded-asset rung and the
	// registry-tag rung. Both build the same authored-surface snapshot first and hand it here, so a record built
	// from a tag and a record built from a load cannot differ: nothing downstream of this point knows which rung it
	// came from. SurfaceDiagnostics carries anything raised while taking the snapshot (a graph that would not
	// compile), which is always empty on the tag rung, since a surface that did not build is never stored.
	static FCrowdyEffectPlanRecord BuildRecordFromSurface(
		const FCrowdyEffectAuthoredSurface& Surface, const TArray<FCrowdyEffectDiagnostic>& SurfaceDiagnostics,
		const FCrowdyEffectAssetProbe& Probe, const TArray<FCrowdyDesiredContainerType>& DesiredTypes);

	TMap<FString, FCrowdyEffectPlanRecord> Records;
	FString Salt;
	bool bSaltSet = false;
};
