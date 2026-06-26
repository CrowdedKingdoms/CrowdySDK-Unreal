// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "Replication/RPC/FCrowdyFnInfo.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyAutoRegistry.generated.h"

class UClass;
class UFunction;

/**
 * Populates the wire-type registries. Payload structs are derived from how
 * they are used — meta=(CrowdyEvent) handler parameters and
 * UActorUpdateExecutor state structs — never from struct annotations.
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
	 * a whole swathe of UFunction pointers is invalidated at once and nothing tracks which — chiefly
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
	 * the class's stale entries by id — without dereferencing the dead pointers — then re-scans just
	 * that class. The editor calls this per recompiled class to keep a running PIE session in step.
	 */
	void UpdateClassRpcFunctions(UClass* Class);

	/**
	 * Gathers the distinct named channels every discovered Multicast CrowdyEvent routes over, so the
	 * channel subsystem can join them all on connect (a client only receives on channels it joined).
	 * bOutUsesDefaultChannel is set when any Multicast leaves its channel unset (the session channel).
	 */
	void CollectMulticastChannels(TSet<FString>& OutChannelNames, bool& bOutUsesDefaultChannel) const;

private:
	void InstallIDOverrideResolvers();
	void ScanAndRegisterExecutorStateStructs();
	void ScanAndRegisterEntityClasses();
	void ScanAndRegisterRpcFunctions();

	// Per-class CrowdyEvent discovery, shared by the full sweep and the incremental update.
	// Adds one class's qualifying handler functions to both maps; skips SKEL_/REINST_ shadows.
	void RegisterClassRpcFunctions(UClass* Class);

	// Drops every entry owned by a class id from both maps. Matches the outbound map by the
	// stored OwnerClassID and the resolver by its class-id key, so it never dereferences a
	// possibly-stale UFunction pointer.
	void RemoveClassRpcFunctions(FCrowdyClassID ClassID);

	FCrowdyClassID ResolveClassID(const UClass* Class) const;

	static bool HasEntityComponent(const UClass* Class);

	// Outbound: send-side lookup of a function's routing/serialization metadata.
	TMap<const UFunction*, FCrowdyFnInfo> RpcFunctionInfo;

	// Inbound: (declaring ClassID, FunctionID) -> receiver UFunction.
	TMap<TPair<int64, int64>, TObjectPtr<UFunction>> RpcFunctionResolver;

#if WITH_EDITOR
	// Live Coding recycles UFunction addresses, invalidating both maps; rebuild
	// when a reload completes.
	FDelegateHandle ReloadCompleteHandle;
#endif
};
