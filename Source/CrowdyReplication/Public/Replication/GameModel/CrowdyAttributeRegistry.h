// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FProperty;

/**
 * One discovered Game Model attribute: a UPROPERTY marked meta=(CrowdyModel) on a CrowdyContainer class.
 * Produced by FCrowdyAttributeRegistry::DiscoverForClass and consumed by (a) the container subsystem's
 * cache/OnRep, (b) the BP read library, and (c) the code-to-server schema sync (which turns each def into a
 * server property definition). Purely descriptive no live property pointer, so a def outlives a discovery pass.
 */
struct FCrowdyAttributeDef
{
	// The UPROPERTY name (e.g. "Health").
	FName PropertyName;

	// The server property key: the lowercased property name (CrowdyGameModelMetaKeys::ServerKeyForProperty),
	// or a meta=(CrowdyKey=...) override (lowercased/trimmed) when the property declares one.
	FString Key;

	// The Game Model value type this property maps to: "int" | "float" | "bool" | "string" | "array" (a scalar
	// array) | "container_ref" (an FCrowdyModelRef) | "object" (a plain native struct). Empty means the property's
	// C++ type is not a supported Server Owned type; it is dropped from discovery with a warning.
	FString ValueType;

	// The server READ-visibility: "public" | "owner" | "hidden" (from meta=(CrowdyVisibility=...)). Defaults
	// "public" (the type default) when the property declares none or an unrecognized value. Consumed by the
	// schema sync's property def; the runtime cache/OnRep does not use it (it is a server access-control concept).
	FString Visibility = TEXT("public");

	// Native ClampMin/ClampMax parsed from the property's metadata. Both must be present for bHasClamp.
	bool bHasClamp = false;
	double ClampMin = 0.0;
	double ClampMax = 0.0;

	// The parameterless CrowdyOnRep notify fired after a pulled/confirmed value is written. NAME_None when the
	// property declares none, or when the named function was dropped because it is missing / not parameterless.
	FName OnRepFunctionName = NAME_None;
};

/**
 * Reflection discovery of Game Model attributes and the single shared FProperty -> Game Model value-type map.
 *
 * Live-metadata only (editor/PIE). Cooked builds strip UPROPERTY metadata, so a shipping path reads a
 * baked attribute table instead (mirroring UCrowdyBakedRegistry for RPC). Without that baked table,
 * discovery returns empty without WITH_METADATA; every caller is PIE/editor-gated.
 *
 * This is where MapPropertyToValueType lives the code-to-server schema sync imports it so the runtime cache
 * and the authored server schema always agree on a property's type. Do not duplicate that mapping elsewhere.
 */
struct CROWDYREPLICATION_API FCrowdyAttributeRegistry
{
	// Maps an FProperty to its Game Model value type: "int" (int/int64/byte), "float" (float/double), "bool",
	// "string" (FString), "array" (a scalar/string array), "container_ref" (an FCrowdyModelRef), "object" (a plain
	// native struct). Returns empty for any other type (a Blueprint struct, an object reference, a name/text, a
	// map/set). THE single source of this mapping (the schema sync and the value codec reuse it).
	static FString MapPropertyToValueType(const FProperty* Property);

	// Reads a class's CrowdyContainer UCLASS tag into OutTypeName. False (OutTypeName untouched) when the
	// class carries no CrowdyContainer meta, or the tag is empty.
	static bool GetContainerTypeName(const UClass* Class, FString& OutTypeName);

	// True when the class declares meta=(CrowdyScope="App"): one row per key for the whole app, bound with no session
	// id. Absent or "Session" is false; any other word warns and is false. Baked for cooked builds.
	static bool IsContainerAppScoped(const UClass* Class);

	// The reverse: the container class whose CrowdyContainer tag is TypeName, or null when nothing declares it. The
	// comparison is case-sensitive, matching the server, which treats a container type name as an exact key. Test
	// fixtures (meta=(CrowdyContainerTest)) are excluded, as they are from every other scan of live container types.
	//
	// In the editor a miss over loaded classes is not an answer: a Blueprint container nothing has opened yet
	// declares its type name only in the asset registry, so the lookup falls back to that tag and loads the one
	// asset claiming the name. That makes it both a full class walk and a possible synchronous load, so it belongs
	// on an authoring / compile path that resolves once and keeps the class, never on a per-frame one.
	static UClass* FindContainerClassByTypeName(const FString& TypeName);

	// True when Class is a test-only container fixture (meta=(CrowdyContainerTest)). Such a class is reflected
	// like any UCLASS, so its CDO exists in an editor build, but it must never be gathered as a real container
	// type for the server schema sync (it would be upserted to a live app as a bogus type). Live metadata only;
	// the cooked path never runs the schema sync, so there is no baked equivalent.
	static bool IsTestContainer(const UClass* Class);

	// True when Class is an SDK test fixture (meta=(CrowdyTestFixture)); false in a cooked build, where metadata is stripped.
	static bool IsTestFixture(const UClass* Class);

	// True when Class has at least one accepted CrowdyModel attribute (a supported value type, not dropped by
	// the plane-exclusivity filter).
	//
	// Deliberately NOT an existence gate on a container. A container whose whole contribution is functions
	// (signals, timers, automations) declares no attribute, because those are authored on an effect asset rather
	// than on the class, so "no attributes" answers nothing about whether the class is a real container. What
	// declares a container is its CrowdyContainer tag, and that is what the schema sync gather and auto-bind read.
	// This remains a query, used to describe a container, never to decide whether it exists.
	static bool ClassHasModelAttributes(const UClass* Class);

	// Discovers every CrowdyModel attribute on Class (including inherited). For each: derives the server key,
	// maps the value type (dropping unsupported types with a warning), reads native ClampMin/ClampMax,
	// resolves the CrowdyOnRep notify (dropping a missing / non-parameterless one with an error, GAS-style),
	// and REJECTS a property that is also marked CrowdyState (a field lives in exactly one plane keeps the two
	// planes from collapsing). Returns the accepted defs; live metadata only.
	static TArray<FCrowdyAttributeDef> DiscoverForClass(const UClass* Class);
};
