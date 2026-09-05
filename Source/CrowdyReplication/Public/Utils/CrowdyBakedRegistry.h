// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/SoftObjectPath.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h" // ECrowdyEventRecipient, ECrowdyDecayRate, ECrowdyReplicationDistance
#include "CrowdyBakedRegistry.generated.h"

class UFunction;
struct FCrowdyRepLayout;
struct FCrowdyAttributeDef;

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

	/** Class that declares the function (ExcludeSuper its owner class). */
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

	// True for meta=(CrowdyAction): the author declares this event's parameters describe a one-shot
	// action. Baked because the metadata it comes from is stripped from cooked builds, and a receiver
	// holding the entity as data has no other way to learn it.
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	bool bIsAction = false;

	// True for a Blueprint "Crowdy Replicates" event (meta=(CrowdyReplicates)). The router
	// uses this to keep a replicated event from also being bound as a struct handler in a
	// cooked build, where the metadata it would otherwise read has been stripped.
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	bool bIsReplicated = false;
};

/**
 * Cooked-safe identity for one meta=(CrowdyState) property in a class's rep layout.
 *
 * The flags and OnRep name come from meta=(...) keys that are stripped from packaged
 * builds, so for a cooked build this bake is the only runtime source. PropertyID and
 * LayoutOrder are pure reflection but are baked so the cooked layout matches what the
 * live builder computes. LayoutOrder is the positional index within the owning class's
 * layout, so the assembled layout preserves wire order.
 */
USTRUCT()
struct FCrowdyBakedRepProperty
{
	GENERATED_BODY()

	/** Class that declares the layout this property belongs to. */
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FSoftClassPath OwnerClassPath;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FName PropertyName;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	int64 PropertyID = 0;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	bool bOwnerOnly = false;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	bool bManualDirty = false;

	// Opt-in keyframe-heartbeat membership. Comes from meta=(CrowdyHeartbeat), stripped from cooked builds,
	// so this bake is the only runtime source there (mirrors bOwnerOnly).
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	bool bHeartbeat = false;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FName OnRepFunctionName = NAME_None;

	// Positional index within the class layout; the assembled layout is ordered by this.
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	int32 LayoutOrder = 0;
};

/**
 * Cooked-safe copy of a class's CrowdyState layout hash. Baked so a cooked layout
 * carries the same positional-order guard the live builder produces, letting a parity
 * test confirm the assembled layout matches.
 */
USTRUCT()
struct FCrowdyBakedRepLayoutHash
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FSoftClassPath ClassPath;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	int64 LayoutHash = 0;
};

/**
 * Cooked-safe identity for one meta=(CrowdyModel) Game Model attribute on a class.
 *
 * The value type, native clamp, and OnRep name are resolved from meta=(...) keys (CrowdyModel,
 * ClampMin/ClampMax, CrowdyOnRep) that are stripped from packaged builds, so for a cooked build this
 * bake is the only runtime source. Mirrors FCrowdyBakedRepProperty for the TRUTH plane: the editor baker
 * reflects FCrowdyAttributeRegistry::DiscoverForClass into these rows so the runtime cache/auto-bind path
 * works with no HasMetaData. The Key is the server property key (lowercased property name by default).
 */
USTRUCT()
struct FCrowdyBakedAttribute
{
	GENERATED_BODY()

	/** Class that declares this attribute (its owning CrowdyContainer class). */
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FSoftClassPath OwnerClassPath;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FName PropertyName;

	// The server property key: the lowercased property name, or the property's CrowdyKey override when set.
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FString Key;

	// "int" | "float" | "bool" | "string" | "array" | "container_ref" | "object" (FCrowdyAttributeRegistry::MapPropertyToValueType).
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FString ValueType;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	bool bHasClamp = false;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	double ClampMin = 0.0;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	double ClampMax = 0.0;

	// The parameterless CrowdyOnRep notify, or NAME_None. Resolved+validated at bake time (live metadata).
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FName OnRepFunctionName = NAME_None;
};

/**
 * Cooked-safe copy of a class's meta=(CrowdyContainer="Type") UCLASS tag. Baked so the cooked auto-bind
 * gate (FCrowdyAttributeRegistry::GetContainerTypeName) can resolve the container type name without the
 * stripped class metadata. One entry per class that carries the CrowdyContainer tag (the baker gates only on
 * the tag, matching GetContainerTypeName, independent of whether the class has any baked attributes).
 */
USTRUCT()
struct FCrowdyBakedModelClass
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FSoftClassPath ClassPath;

	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	FString ContainerTypeName;

	// Whether a container of this class fetches its server state once as soon as it binds. Resolved at bake time
	// from meta=(CrowdyPullOnStart) on this class or, when it declares none, on the nearest ancestor that does, so
	// the cooked read needs no metadata. Defaults to true, which is also what an absent tag means, so a registry
	// baked before this flag existed reads as "pulls" for every class in it.
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	bool bPullOnStart = true;
};

/**
 * Cooked-safe snapshot of the editor-only Crowdy metadata.
 *
 * The SDK authors event handlers, listeners, and persistent structs with
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

	/** Every meta=(CrowdyState) property, one entry each, grouped per class by OwnerClassPath. */
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	TArray<FCrowdyBakedRepProperty> RepProperties;

	/** The CrowdyState layout hash for every class that declares any, one entry each. */
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	TArray<FCrowdyBakedRepLayoutHash> RepLayoutHashes;

	/** Every meta=(CrowdyModel) Game Model attribute, one entry each, grouped per class by OwnerClassPath. */
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	TArray<FCrowdyBakedAttribute> ModelAttributes;

	/** The CrowdyContainer type-name tag for every CrowdyContainer-tagged class, one entry each. */
	UPROPERTY(VisibleAnywhere, Category = "Crowdy SDK")
	TArray<FCrowdyBakedModelClass> ModelClasses;

	// Runtime query surface
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

	/**
	 * Baked CrowdyState properties for a class, ordered by LayoutOrder, or null if the
	 * class declared none. This always reads the baked asset (the live equivalent is
	 * assembled by FCrowdyStateLayoutBuilder), so it is the cooked-build source of the layout.
	 */
	const TArray<FCrowdyBakedRepProperty>* FindRepProperties(const FSoftClassPath& OwnerClassPath) const;

	/** Baked CrowdyState layout hash for a class, or 0 if the class declared none. */
	int64 FindRepLayoutHash(const FSoftClassPath& OwnerClassPath) const;

	/** Static convenience that reads Get(); mirrors the static FindRpcFunction(const UFunction*). */
	static const TArray<FCrowdyBakedRepProperty>* FindRepProperties(const UClass* Class);
	static int64 FindRepLayoutHash(const UClass* Class);

	/**
	 * Baked Game Model attributes for a class, or null if the class declared none. This always reads the
	 * baked asset (the live equivalent is FCrowdyAttributeRegistry::DiscoverForClass), so it is the
	 * cooked-build source of the attribute set. Mirrors FindRepProperties for the TRUTH plane.
	 */
	const TArray<FCrowdyBakedAttribute>* FindModelAttributes(const FSoftClassPath& OwnerClassPath) const;
	static const TArray<FCrowdyBakedAttribute>* FindModelAttributes(const UClass* Class);

	/**
	 * The baked CrowdyContainer type name for a class. Returns false (OutTypeName untouched) when the class
	 * carries no baked tag. Cooked-build source of FCrowdyAttributeRegistry::GetContainerTypeName.
	 */
	bool FindContainerTypeName(const FSoftClassPath& ClassPath, FString& OutTypeName) const;
	static bool FindContainerTypeName(const UClass* Class, FString& OutTypeName);

	/**
	 * Whether a Game Model container of this class fetches its server state once as soon as it binds.
	 *
	 * True unless the class, or the nearest ancestor that declares the tag, carries meta=(CrowdyPullOnStart="False").
	 * An absent tag, an unknown class, and a null class all mean true, so a container that never opts out needs no
	 * tag at all and nothing that predates the tag changes meaning.
	 *
	 * Class metadata is not inherited (UStruct::HasMetaData answers for one class only), so the live path walks the
	 * super chain itself; the baked path walks the same chain over the baked container entries, whose values the
	 * baker already resolved through that walk. The two agree for any container class and for anything derived from
	 * one, which is the only thing the bind gate asks about.
	 */
	static bool ShouldPullModelOnStart(const UClass* Class);

	/**
	 * The baked pull-on-start flag recorded for exactly this class, ignoring its supers. Returns false (and leaves
	 * bOutPullOnStart untouched) when the class carries no baked container entry. The hierarchy walk lives in
	 * ShouldPullModelOnStart; this is the single-entry read it and the tests are built on.
	 */
	bool FindPullModelOnStart(const FSoftClassPath& ClassPath, bool& bOutPullOnStart) const;

	/**
	 * Pure live->baked factory: appends one FCrowdyBakedRepProperty per layout property
	 * (with OwnerClassPath set to OwnerClassPath and LayoutOrder = its index), never clearing
	 * OutProps. Used by both the editor baker and the bake round-trip test.
	 */
	static void MakeBakedRepProperties(const FCrowdyRepLayout& Layout, const FSoftClassPath& OwnerClassPath, TArray<FCrowdyBakedRepProperty>& OutProps);

	/**
	 * Pure live->baked factory: appends one FCrowdyBakedAttribute per discovered attribute def
	 * (with OwnerClassPath set), never clearing OutAttrs. Used by both the editor baker and the bake
	 * round-trip test. Mirrors MakeBakedRepProperties.
	 */
	static void MakeBakedAttributes(const TArray<FCrowdyAttributeDef>& Defs, const FSoftClassPath& OwnerClassPath, TArray<FCrowdyBakedAttribute>& OutAttrs);

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

	// OwnerClassPath -> its CrowdyState properties, grouped and kept in LayoutOrder.
	mutable TMap<FSoftClassPath, TArray<FCrowdyBakedRepProperty>> RepPropertyLookup;

	// ClassPath -> its CrowdyState layout hash.
	mutable TMap<FSoftClassPath, int64> RepLayoutHashLookup;

	// OwnerClassPath -> its Game Model attributes, grouped.
	mutable TMap<FSoftClassPath, TArray<FCrowdyBakedAttribute>> ModelAttributeLookup;

	// ClassPath -> its baked container entry (type name + pull-on-start).
	mutable TMap<FSoftClassPath, FCrowdyBakedModelClass> ModelClassLookup;
};
