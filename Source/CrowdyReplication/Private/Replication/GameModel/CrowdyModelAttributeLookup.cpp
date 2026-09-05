// Fill out your copyright notice in the Description page of Project Settings.

#include "Replication/GameModel/CrowdyModelAttributeLookup.h"

#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/State/CrowdyStateMetaKeys.h"
#include "UObject/Class.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectGlobals.h" // FCoreUObjectDelegates, EReloadCompleteReason
#include "UObject/UnrealType.h"
#include "Utils/CrowdyBakedRegistry.h" // cooked-build attribute table (metadata is stripped)

namespace
{
	struct FCrowdyModelAttributeTable
	{
		// Weak so a cached table never keeps a container class alive, and checked on every hit so a table is
		// never served for a class that has since been torn down.
		TWeakObjectPtr<const UClass> OwnerClass;

		TMap<FName, FCrowdyModelAttributeEntry> ByServerKey;
	};

	TMap<FObjectKey, FCrowdyModelAttributeTable> GModelAttributeTables;

	FDelegateHandle GModelAttributeReloadHandle;

	// Collects one class's server key to attribute mapping using exactly the rules the apply path used before
	// the table existed, so a cached answer and a direct walk cannot disagree.
	void BuildModelAttributeTable(const UClass* Class, TMap<FName, FCrowdyModelAttributeEntry>& Out)
	{
		Out.Reset();
		if (!Class)
		{
			return;
		}

#if WITH_METADATA
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Property = *It;
			// Match DiscoverForClass exactly: a CrowdyModel attribute that is NOT also CrowdyState (a field lives
			// in exactly one plane) and whose type is a supported Server Owned leaf. Without these two guards the
			// live path resolves a SUPERSET of the baked table, so a malformed dual-marked attribute would take a
			// server write onto a CrowdyState-managed member in the editor (plane collapse) yet be a no-op cooked.
			if (!CrowdyGameModelMetaKeys::HasModelMeta(Property)
				|| Property->HasMetaData(CrowdyStateMetaKeys::Replicate)
				|| FCrowdyAttributeRegistry::MapPropertyToValueType(Property).IsEmpty())
			{
				continue;
			}

			// Resolved through the same helper the schema side uses, so a CrowdyKey override names the same
			// attribute on both ends. Deriving it from the property name here instead meant an overridden
			// attribute was addressed by one key on the server and another by this path, so its value silently
			// never reached the member in a build with metadata.
			const FName ServerKey(*CrowdyGameModelMetaKeys::ResolvedServerKeyForProperty(Property));
			// Field iteration reaches the most-derived declaration first, and discovery keeps the first property
			// it sees for a key and drops later ones, so first-wins here matches which attribute the server has.
			if (Out.Contains(ServerKey))
			{
				continue;
			}

			FCrowdyModelAttributeEntry& Entry = Out.Add(ServerKey);
			Entry.Property = Property;
			const FString OnRep = Property->GetMetaData(CrowdyStateMetaKeys::OnRep);
			Entry.OnRepFunctionName = OnRep.IsEmpty() ? NAME_None : FName(*OnRep);
		}
#else
		// Cooked builds: metadata is stripped, so the server-key -> property mapping comes from the baked
		// attribute table. The baked Key/PropertyName are exactly what live discovery produced, so
		// FindPropertyByName re-resolves the same FProperty the WITH_METADATA path returns, and the notify name
		// was resolved + validated at bake time from the same live metadata.
		if (const TArray<FCrowdyBakedAttribute>* Attrs = UCrowdyBakedRegistry::FindModelAttributes(Class))
		{
			for (const FCrowdyBakedAttribute& Attr : *Attrs)
			{
				const FName ServerKey(*Attr.Key);
				if (Out.Contains(ServerKey))
				{
					continue;
				}

				FCrowdyModelAttributeEntry& Entry = Out.Add(ServerKey);
				Entry.Property = Class->FindPropertyByName(Attr.PropertyName);
				Entry.OnRepFunctionName = Attr.OnRepFunctionName;
			}
		}
#endif
	}

	const FCrowdyModelAttributeTable& ResolveModelAttributeTable(const UClass* Class)
	{
		const FObjectKey Key(Class);
		const FCrowdyModelAttributeTable* Cached = GModelAttributeTables.Find(Key);
		// The weak owner is the second half of the identity check: it stops a table being served for a class
		// that is on its way out, whose reflection data may already have been torn down.
		if (Cached && Cached->OwnerClass.Get() == Class)
		{
			return *Cached;
		}

		// Built into a local first. Building reads reflection and, in a cooked build, can load the baked
		// registry asset, which runs arbitrary load-time code; if any of that ever resolved a table for a
		// second class it would grow this map and move the element being written to. Inserting only once the
		// contents are final also means an entry, once stored, is never mutated again.
		TMap<FName, FCrowdyModelAttributeEntry> Built;
		BuildModelAttributeTable(Class, Built);

		FCrowdyModelAttributeTable& Table = GModelAttributeTables.Add(Key);
		Table.OwnerClass = Class;
		Table.ByServerKey = MoveTemp(Built);
		return Table;
	}
}

const FCrowdyModelAttributeEntry* FCrowdyModelAttributeLookup::Find(const UClass* Class, const FName ServerKey)
{
	if (!Class)
	{
		return nullptr;
	}
	return ResolveModelAttributeTable(Class).ByServerKey.Find(ServerKey);
}

FProperty* FCrowdyModelAttributeLookup::FindProperty(const UObject* Container, const FName ServerKey)
{
	if (!Container)
	{
		return nullptr;
	}
	const FCrowdyModelAttributeEntry* Entry = Find(Container->GetClass(), ServerKey);
	return Entry ? Entry->Property : nullptr;
}

FName FCrowdyModelAttributeLookup::FindOnRep(const UObject* Container, const FName ServerKey)
{
	if (!Container)
	{
		return NAME_None;
	}
	const FCrowdyModelAttributeEntry* Entry = Find(Container->GetClass(), ServerKey);
	return Entry ? Entry->OnRepFunctionName : NAME_None;
}

void FCrowdyModelAttributeLookup::InvalidateClass(const UClass* Class)
{
	if (!Class)
	{
		return;
	}
	// Removed by key, never by reading the entry: a recompile has already destroyed the properties it holds.
	GModelAttributeTables.Remove(FObjectKey(Class));
}

void FCrowdyModelAttributeLookup::InvalidateAll()
{
	GModelAttributeTables.Reset();
}

void FCrowdyModelAttributeLookup::Install()
{
#if WITH_EDITOR
	if (GModelAttributeReloadHandle.IsValid())
	{
		return;
	}
	// Live Coding rebuilds native reflection and recycles FProperty addresses across many classes at once, so
	// every cached table goes stale together.
	GModelAttributeReloadHandle = FCoreUObjectDelegates::ReloadCompleteDelegate.AddLambda(
		[](EReloadCompleteReason)
		{
			FCrowdyModelAttributeLookup::InvalidateAll();
		});
#endif
}

void FCrowdyModelAttributeLookup::Uninstall()
{
#if WITH_EDITOR
	if (GModelAttributeReloadHandle.IsValid())
	{
		FCoreUObjectDelegates::ReloadCompleteDelegate.Remove(GModelAttributeReloadHandle);
		GModelAttributeReloadHandle.Reset();
	}
#endif
	InvalidateAll();
}

int32 FCrowdyModelAttributeLookup::NumCachedClasses()
{
	return GModelAttributeTables.Num();
}

#if WITH_DEV_AUTOMATION_TESTS
bool FCrowdyModelAttributeLookup::OverwriteEntryForTest(const UClass* Class, const FName ServerKey,
	FProperty* Property, const FName OnRepFunctionName)
{
	if (!Class)
	{
		return false;
	}
	FCrowdyModelAttributeTable* Table = GModelAttributeTables.Find(FObjectKey(Class));
	if (!Table)
	{
		return false;
	}
	FCrowdyModelAttributeEntry* Entry = Table->ByServerKey.Find(ServerKey);
	if (!Entry)
	{
		return false;
	}
	Entry->Property = Property;
	Entry->OnRepFunctionName = OnRepFunctionName;
	return true;
}

bool FCrowdyModelAttributeLookup::IsClassCachedForTest(const UClass* Class)
{
	return Class && GModelAttributeTables.Contains(FObjectKey(Class));
}
#endif
