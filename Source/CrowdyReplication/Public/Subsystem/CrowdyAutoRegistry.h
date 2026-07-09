// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "Replication/RPC/FCrowdyFnInfo.h"
#include "Replication/State/FCrowdyRepLayout.h" // FCrowdyRepLayout is a complete type in the TUniquePtr map value
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyAutoRegistry.generated.h"

class UClass;
class UFunction;
class UCrowdyBakedRegistry;

/**
 * Populates the wire-type registries. Payload structs are derived from how
 * they are used meta=(CrowdyEvent) handler parameters and
 * UActorUpdateExecutor state structs, never from struct annotations.
 * The scans run at startup and again on every world init (see
 * RegisterLoadedPayloadTypes) so level transitions pick up the classes that
 * loaded with the new map.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyAutoRegistry : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Scans every loaded class for CrowdyEvent handler parameters and executor
	 * state structs and registers them. Called at startup and per world by
	 * UCrowdyEventRouter; cheap to repeat (duplicates are no-ops).
	 */
	void RegisterLoadedPayloadTypes();

	/**
	 * Send-side routing/serialization metadata for a CrowdyEvent function, or null
	 * if the function was not discovered. O(1).
	 */
	const FCrowdyFnInfo* FindFunctionInfo(const UFunction* Function) const;

	/**
	 * Receive-side resolution: the UFunction for a declaring-class id + function id
	 * pair carried on an inbound FCrowdyRpcCall, or null if not discovered (e.g. the
	 * declaring class is not loaded). O(1).
	 */
	UFunction* ResolveFunction(int64 ClassID, int64 FunctionID) const;

	/**
	 * Rebuilds the CrowdyEvent function maps wholesale from every currently-loaded class. Used when
	 * a whole swathe of UFunction pointers is invalidated at once and nothing tracks which chiefly
	 * a Live Coding reload, which recycles native function addresses across many classes. A single
	 * Blueprint compile instead reinstances just one class, so the editor refreshes only that class
	 * via UpdateClassRpcFunctions rather than paying this full sweep. Safe to call repeatedly.
	 */
	void RescanRpcFunctions();

	/**
	 * Incrementally refreshes a single class's CrowdyEvent entries after it recompiles, instead of
	 * resweeping every loaded class (the source of the per-compile editor hitch). Compiling one
	 * Blueprint reinstances only its class, so its old UFunction pointers go stale while every other
	 * class's stay valid. The class id is path-derived and stable across recompiles, so this evicts
	 * the class's stale entries by id without dereferencing the dead pointers then re-scans just
	 * that class. The editor calls this per recompiled class to keep a running PIE session in step.
	 */
	void UpdateClassRpcFunctions(UClass* Class);

	/**
	 * Gathers the distinct named channels every discovered Multicast CrowdyEvent routes over, so the
	 * channel subsystem can join them all on connect (a client only receives on channels it joined).
	 * bOutUsesDefaultChannel is set when any Multicast leaves its channel unset (the session channel).
	 */
	void CollectMulticastChannels(TSet<FString>& OutChannelNames, bool& bOutUsesDefaultChannel) const;

	/**
	 * Assembles a live FCrowdyRepLayout for a class from the baked rep table (the cooked-build path,
	 * where UPROPERTY metadata is stripped so the live builder yields nothing). Reads the class's baked
	 * rep properties in LayoutOrder, re-resolves each FProperty* by name against the class, and copies
	 * the frozen PropertyID/flags/OnRep name plus the baked LayoutHash. Returns Out.IsValid() false
	 * when the class has no baked rep properties. Static so tests can round-trip a baked table without
	 * a registry instance. Mirrors FCrowdyStateLayoutBuilder::BuildLayout, whose output it reproduces.
	 */
	static bool BuildLayoutFromBaked(const UClass* Class, const UCrowdyBakedRegistry* Baked, FCrowdyRepLayout& Out);

	/**
	 * Incrementally refreshes a single class's CrowdyState rep layout after it recompiles, instead of
	 * resweeping every loaded class. Compiling one Blueprint reinstances only its class, invalidating
	 * the FProperty pointers cached in that class's layout while every other class's stay valid. The
	 * class id is path-derived and stable across recompiles, so this evicts the class's cached layout by
	 * id without dereferencing the dead pointers then rebuilds just that class. The editor calls
	 * this per recompiled class. Mirrors UpdateClassRpcFunctions.
	 */
	void UpdateClassRepLayout(UClass* Class);

	/**
	 * Rebuilds every class's CrowdyState rep layout wholesale from the currently-loaded classes. The
	 * layout builder path caches raw FProperty pointers, so a Live Coding reload (which recycles those
	 * addresses across many classes) invalidates them all at once; this is the matching full rescan.
	 * A single Blueprint compile instead refreshes only the recompiled class via UpdateClassRepLayout.
	 * Also the public scan entry point for tests. Mirrors RescanRpcFunctions. Safe to call repeatedly.
	 */
	void RescanRepLayouts();

	/**
	 * CrowdyState rep layout for a class, or null if the class has no CrowdyState properties. Builds it
	 * live from reflection in the editor (WITH_METADATA) or assembled from the baked table in a cooked
	 * build, then caches it; both paths yield an identical layout. The cache stores layouts in a
	 * pointer-stable map, so the returned pointer stays valid across later inserts (Phase 3 caches it
	 * per owned entity). Negative results are never cached. Const; lazily builds and caches on first use.
	 */
	const FCrowdyRepLayout* FindRepLayout(const UClass* Class) const;

	/**
	 * Resolves an actor class's default UCrowdyEntityComponent instance (native CDO or BP SCS component
	 * template), mirroring HasEntityComponent's chain walk. Read-only, live reflection only never baked
	 * or cached; intended for editor tooling (e.g. the CrowdyStudio registry inspector) that wants to
	 * show a class's configured Ownership/HostOverride defaults. Returns null for non-actor classes or
	 * actors with no entity component. Never forces CDO creation (GetDefaultObject(false)).
	 */
	static const class UCrowdyEntityComponent* ResolveDefaultEntityComponent(const UClass* ActorClass);

private:
	void InstallIDOverrideResolvers();
	void ScanAndRegisterExecutorStateStructs();
	void ScanAndRegisterEntityClasses();
	void ScanAndRegisterRpcFunctions();

	// Full CrowdyState rep-layout sweep: resets the cache and rebuilds every loaded class's layout.
	void ScanAndRegisterRepLayouts();

	// Per-class CrowdyEvent discovery, shared by the full sweep and the incremental update.
	// Adds one class's qualifying handler functions to both maps; skips SKEL_/REINST_ shadows.
	void RegisterClassRpcFunctions(UClass* Class);

	// Drops every entry owned by a class id from both maps. Matches the outbound map by the
	// stored OwnerClassID and the resolver by its class-id key, so it never dereferences a
	// possibly-stale UFunction pointer.
	void RemoveClassRpcFunctions(FCrowdyClassID ClassID);

	// Per-class CrowdyState discovery, shared by the full sweep and the incremental update. Builds a
	// live layout for one class and, if it has any CrowdyState properties, caches it under the class's
	// stable id; skips SKEL_/REINST_ shadows.
	void RegisterClassRepLayout(UClass* Class);

	// Drops a class's cached rep layout by its stable id, so an incremental rebuild never dereferences
	// the FProperty pointers a recompile invalidated.
	void RemoveClassRepLayout(FCrowdyClassID ClassID);

	FCrowdyClassID ResolveClassID(const UClass* Class) const;

	static bool HasEntityComponent(const UClass* Class);

	// Resolves the executor state struct for an actor class by finding its UCrowdyEntityComponent (native
	// CDO or BP SCS template, mirroring HasEntityComponent) and reading StateExecutor->GetStateStruct().
	// Returns null for non-actors, actors without an entity component/executor, or an executor with no
	// state struct. Never forces CDO creation (GetDefaultObject(false)).
	UScriptStruct* ResolveExecutorStateStruct(const UClass* ActorClass) const;

	// Drops any layout property whose (name, canonical type) also appears in the class's executor state
	// struct, logs an error naming the property + class, and recomputes LayoutHash over the survivors.
	// No-op when there is no executor state struct. Called on every built layout before it is cached
	// (both FindRepLayout's lazy-build path and RegisterClassRepLayout's eager path).
	void ApplyExecutorOverlapFilter(FCrowdyRepLayout& Layout) const;

	// Clears the OnRep binding of any layout property whose CrowdyOnRep names a function that is missing or
	// not parameterless (a notify cannot receive parameters and has no "previous value" on this plane),
	// logging an error naming the function + class. The property still replicates; only its notify is
	// dropped, so LayoutHash is unchanged. Reflection-only (functions survive cooking), so it runs
	// identically on the live and baked paths. Called on every built layout right after
	// ApplyExecutorOverlapFilter.
	void ValidateOnRepSignatures(FCrowdyRepLayout& Layout) const;

	// Outbound: send-side lookup of a function's routing/serialization metadata.
	TMap<const UFunction*, FCrowdyFnInfo> RpcFunctionInfo;

	// Inbound: (declaring ClassID, FunctionID) -> receiver UFunction.
	TMap<TPair<int64, int64>, TObjectPtr<UFunction>> RpcFunctionResolver;

	// CrowdyState rep layout per class, keyed by the stable, path-derived class id so an incremental
	// rebuild evicts by id (surviving BP reinstance) without touching the stale FProperty pointers a
	// recompile left inside the layout. The value is a TUniquePtr so the FCrowdyRepLayout is heap-stable
	// and the const pointer FindRepLayout returns stays valid across later inserts (Phase 3 caches that
	// pointer per owned entity). Mutable because FindRepLayout is const and lazily builds/caches.
	mutable TMap<FCrowdyClassID, TUniquePtr<FCrowdyRepLayout>> RepLayouts;

#if WITH_EDITOR
	// Live Coding recycles UFunction addresses, invalidating both maps; rebuild
	// when a reload completes.
	FDelegateHandle ReloadCompleteHandle;
#endif
};
