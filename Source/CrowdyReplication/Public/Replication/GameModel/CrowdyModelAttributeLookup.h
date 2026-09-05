// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"

class FProperty;
class UClass;

/**
 * One Server Owned attribute of a container class, resolved from its server property key.
 */
struct FCrowdyModelAttributeEntry
{
	// The reflected member an authoritative server value is written onto. Null only in a cooked build whose
	// baked row names a property the class no longer declares, matching what a direct name lookup returns.
	FProperty* Property = nullptr;

	// The parameterless notify to fire after the write, or NAME_None when the attribute declares none.
	FName OnRepFunctionName = NAME_None;
};

/**
 * Server property key to attribute lookup for Game Model container classes.
 *
 * Resolving a key without this meant walking every reflected property of the container's class and building a
 * lowercased copy of each property name to compare, once to find the member and again to find its notify. The
 * apply path does that for every key the server returns, so the walk repeated per key on every pull and every
 * confirmed invoke. The table below is built once per class and answers both questions from one entry.
 *
 * The table holds raw FProperty pointers, so it is only valid while the class's reflection data is. Three things
 * invalidate it and all three are handled:
 *
 *   - Recompiling a Blueprint keeps the class object but destroys and rebuilds its properties. The class object
 *     is unchanged, so nothing about the key would detect it. The editor drops every table when any Blueprint's
 *     class layout is rebuilt. Every table, not just the recompiled class's: a table is built by walking the
 *     class and its supers, so it can hold properties owned by an ancestor, and a derived class is relinked
 *     rather than recompiled and so is never announced on its own.
 *   - Live Coding rebuilds native reflection across many classes at once. Install binds the engine's
 *     reload-complete delegate to drop every table, and Uninstall removes that binding when the module unloads.
 *   - Editing a variable's Crowdy settings writes metadata straight onto the live property without a compile,
 *     so the class, the property and the key are all unchanged while the answer is not. The variable details
 *     panel drops the tables after each such write.
 *
 * Entries are keyed by FObjectKey rather than a raw UClass pointer. An FObjectKey carries the object's serial
 * number as well as its slot, so an address that garbage collection later hands to a different class resolves
 * to a clean miss instead of quietly serving the previous class's properties.
 *
 * Game thread only. Every caller runs on the game thread (the Game Model apply path is driven from client
 * completions that are marshalled there), so the table is deliberately unsynchronized, matching the CrowdyState
 * rep-layout cache. A future off-thread caller would need locking added here first.
 */
class CROWDYREPLICATION_API FCrowdyModelAttributeLookup
{
public:
	/**
	 * The attribute a container class exposes under a server property key, or null when the class has none.
	 * Builds and caches the class's table on first use. The returned pointer is owned by the cache and is
	 * invalidated by the next invalidation or rebuild, so read it out rather than storing it.
	 */
	static const FCrowdyModelAttributeEntry* Find(const UClass* Class, FName ServerKey);

	// The member a server key writes onto for this container, or null when the key names no attribute.
	static FProperty* FindProperty(const UObject* Container, FName ServerKey);

	// The parameterless notify declared on the attribute a server key names, or NAME_None.
	static FName FindOnRep(const UObject* Container, FName ServerKey);

	/**
	 * Drops one class's table. Only correct when the class's own properties are the only ones affected;
	 * a table can hold properties owned by an ancestor, so anything that rebuilds reflection data should
	 * prefer InvalidateAll.
	 */
	static void InvalidateClass(const UClass* Class);

	// Drops every table. Called when reflection data is rebuilt or re-marked anywhere.
	static void InvalidateAll();

	// Binds the reload-complete delegate. Call once when the module starts.
	static void Install();

	// Removes the reload-complete binding and drops every table. Call when the module shuts down.
	static void Uninstall();

	// Number of classes with a cached table. Diagnostics and tests only.
	static int32 NumCachedClasses();

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Replaces a cached entry with the given values so a test can tell a served cache hit from a fresh walk.
	 * Returns false when the class has no cached table or no entry for the key. Test-only: the values are
	 * written verbatim and are never validated.
	 */
	static bool OverwriteEntryForTest(const UClass* Class, FName ServerKey, FProperty* Property, FName OnRepFunctionName);

	// True when the class has a cached table.
	static bool IsClassCachedForTest(const UClass* Class);
#endif
};
