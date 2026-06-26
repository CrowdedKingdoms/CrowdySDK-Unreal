// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/SoftObjectPath.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h" // ECrowdyEventRecipient, ECrowdyDecayRate, ECrowdyReplicationDistance
#include "CrowdyBakedRegistry.generated.h"

class UFunction;

/**
 * Fixed location of the baked registry. The editor baker writes here and the
 * cook injects this package, so the runtime can load it by path with no reliance
 * on a saved soft-pointer / config value.
 */
namespace CrowdyBakedRegistryPaths
{
	inline const TCHAR* PackagePath = TEXT("/Game/CrowdySDK/CrowdyBakedRegistry");
	inline const TCHAR* ObjectPath  = TEXT("/Game/CrowdySDK/CrowdyBakedRegistry.CrowdyBakedRegistry");
}

/**
 * Cooked-safe routing/identity for one CrowdyEvent (RPC) function.
 *
 * FunctionID and bParamsPOD are pure reflection over the signature and could be
 * recomputed at runtime, but are baked too so the cooked values are frozen at
 * cook time and a parity test can confirm they match what the runtime computes.
 * Recipient/DecayRate/Distance come from meta=(...) keys that are stripped from
 * packaged builds, so for those the bake is the only runtime source.
 */
USTRUCT()
struct FCrowdyBakedRpcFunction
{
	GENERATED_BODY()

	/** Class that declares the function (ExcludeSuper — its owner class). */
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FSoftClassPath ClassPath;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FName FunctionName;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	int64 FunctionID = 0;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	ECrowdyEventRecipient Recipient = ECrowdyEventRecipient::SpatialMulticast;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	ECrowdyReplicationDistance Distance = ECrowdyReplicationDistance::Eight_Chunks;

	// For a Multicast: the channel name it routes over (empty = default session channel). Baked
	// because the meta it comes from is stripped from cooked builds.
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FString ChannelName;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	bool bParamsPOD = false;

	// True for a Blueprint "Crowdy Replicates" event (meta=(CrowdyReplicates)). The router
	// uses this to keep a replicated event from also being bound as a struct handler in a
	// cooked build, where the metadata it would otherwise read has been stripped.
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	bool bIsReplicated = false;
};

/**
 * Cooked-safe snapshot of the editor-only Crowdy metadata.
 *
 * The SDK authors event handlers, listeners and persistent structs with
 * meta=(...) keys. Metadata is stripped from packaged builds (WITH_METADATA==0),
 * so the editor baker (UCrowdyRegistryBaker) reads that metadata while it still
 * exists and writes it into this asset, which cooks normally. At runtime the
 * query helpers below read live metadata in the editor and this baked data in a
 * packaged build, so behaviour is identical across both.
 *
 * The asset is assigned in UCrowdySDKDeveloperSettings::BakedRegistry.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyBakedRegistry : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Structs carrying meta=(CrowdyPersistent). */
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	TArray<FSoftObjectPath> PersistentStructs;

	/** Structs carrying meta=(CrowdySingleton). */
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	TArray<FSoftObjectPath> SingletonStructs;

	/** Routing/identity for every meta=(CrowdyEvent) function, one entry each. */
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	TArray<FCrowdyBakedRpcFunction> RpcFunctions;

	// ── Runtime query surface ────────────────────────────────────────────────
	// Each helper reads live metadata when WITH_METADATA is available (editor /
	// PIE) and the baked snapshot otherwise (packaged build). Call sites stay
	// metadata-agnostic.

	static bool IsEventHandlerFunction(const UFunction* Function);
	static bool IsReplicatedEventFunction(const UFunction* Function);
	static bool IsPersistentStruct(const UScriptStruct* Struct);
	static bool IsSingletonStruct(const UScriptStruct* Struct);

	/**
	 * Baked routing for a CrowdyEvent function, looked up by its declaring class
	 * and name, or null if the function was not baked. Unlike the Is* helpers this
	 * always reads the baked asset (the live equivalent is assembled by
	 * FCrowdyRPC::BuildFnInfo), so it is the cooked-build source of routing.
	 */
	static const FCrowdyBakedRpcFunction* FindRpcFunction(const UFunction* Function);

	/** Asset-local variant used by FindRpcFunction and by tests. */
	const FCrowdyBakedRpcFunction* FindRpcFunction(const FSoftClassPath& OwnerClassPath, FName FunctionName) const;

	/** Loads (and caches) the asset configured in UCrowdySDKDeveloperSettings. */
	static const UCrowdyBakedRegistry* Get();

	/** Drops the cached pointer; call when the asset is rebaked in the editor. */
	static void InvalidateCache();

private:
	// Lazily-built O(1) lookups over the path arrays.
	void BuildLookups() const;

	mutable bool bLookupsBuilt = false;
	mutable TSet<FSoftObjectPath> PersistentLookup;
	mutable TSet<FSoftObjectPath> SingletonLookup;

	// (declaring class, function name) -> index into RpcFunctions.
	mutable TMap<TPair<FSoftClassPath, FName>, int32> RpcFunctionLookup;
};
