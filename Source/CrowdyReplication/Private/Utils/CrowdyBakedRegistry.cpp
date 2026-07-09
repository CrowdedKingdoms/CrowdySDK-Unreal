// Fill out your copyright notice in the Description page of Project Settings.

#include "Utils/CrowdyBakedRegistry.h"
#include "CrowdyReplicationLog.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Replication/State/FCrowdyRepLayout.h" // FCrowdyRepLayout, FCrowdyRepProperty
#include "UObject/Class.h"          // UFunction, UClass
#include "UObject/UObjectGlobals.h"

namespace
{
	// Cached singleton. The asset is referenced by the developer settings soft
	// pointer, so it cooks; we keep our own root reference so a GC pass between
	// load and use cannot collect it.
	TWeakObjectPtr<const UCrowdyBakedRegistry> GCachedRegistry;
	bool bCacheResolved = false;
}

const UCrowdyBakedRegistry* UCrowdyBakedRegistry::Get()
{
	if (bCacheResolved && GCachedRegistry.IsValid())
		return GCachedRegistry.Get();

	bCacheResolved = true;

	// Optional explicit override via settings, otherwise the fixed path the cook
	// injects. The path fallback means a project never needs to assign anything.
	UCrowdyBakedRegistry* Loaded = nullptr;
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
		Loaded = Settings->BakedRegistry.LoadSynchronous();

	if (!Loaded)
		Loaded = LoadObject<UCrowdyBakedRegistry>(nullptr, CrowdyBakedRegistryPaths::ObjectPath);

	if (Loaded)
	{
		Loaded->AddToRoot();
		GCachedRegistry = Loaded;
	}
	else
	{
#if !WITH_METADATA
		// Only worth warning in a build that actually depends on the bake.
		UE_LOG(LogCrowdyReplication, Warning,
			TEXT("[CrowdyBakedRegistry] No baked registry assigned in Crowdy SDK settings. "
			     "Event handlers, listeners and persistent structs will not be discovered. "
			     "Run 'Rebuild Crowdy Registry' in the editor and assign the asset."));
#endif
	}

	return GCachedRegistry.Get();
}

void UCrowdyBakedRegistry::InvalidateCache()
{
	if (UObject* Pinned = const_cast<UCrowdyBakedRegistry*>(GCachedRegistry.Get()))
		Pinned->RemoveFromRoot();
	GCachedRegistry.Reset();
	bCacheResolved = false;
}

void UCrowdyBakedRegistry::BuildLookups() const
{
	if (bLookupsBuilt) return;

	PersistentLookup.Append(PersistentStructs);
	SingletonLookup.Append(SingletonStructs);

	RpcFunctionLookup.Reserve(RpcFunctions.Num());
	for (int32 Index = 0; Index < RpcFunctions.Num(); ++Index)
	{
		const FCrowdyBakedRpcFunction& Entry = RpcFunctions[Index];
		RpcFunctionLookup.Add(TPair<FSoftClassPath, FName>(Entry.ClassPath, Entry.FunctionName), Index);
	}

	// Group the flat rep-property list by owning class. The baked array is emitted sorted by
	// (OwnerClassPath, LayoutOrder), so appending in array order keeps each class's group in
	// LayoutOrder; sort each group defensively in case the asset was written out of order.
	for (const FCrowdyBakedRepProperty& Prop : RepProperties)
	{
		RepPropertyLookup.FindOrAdd(Prop.OwnerClassPath).Add(Prop);
	}
	for (TPair<FSoftClassPath, TArray<FCrowdyBakedRepProperty>>& Group : RepPropertyLookup)
	{
		Group.Value.Sort([](const FCrowdyBakedRepProperty& A, const FCrowdyBakedRepProperty& B)
		{
			return A.LayoutOrder < B.LayoutOrder;
		});
	}

	RepLayoutHashLookup.Reserve(RepLayoutHashes.Num());
	for (const FCrowdyBakedRepLayoutHash& Entry : RepLayoutHashes)
	{
		RepLayoutHashLookup.Add(Entry.ClassPath, Entry.LayoutHash);
	}

	bLookupsBuilt = true;
}

bool UCrowdyBakedRegistry::IsEventHandlerFunction(const UFunction* Function)
{
	if (!Function) return false;
#if WITH_METADATA
	return Function->HasMetaData(TEXT("CrowdyEvent"));
#else
	// Cooked builds: metadata is stripped, so "is this a CrowdyEvent function" is answered from
	// the bake. Every CrowdyEvent function has exactly one RpcFunctions entry (both are baked from
	// the same scan), so a hit there is the same answer the per-class handler list used to give.
	return FindRpcFunction(Function) != nullptr;
#endif
}

bool UCrowdyBakedRegistry::IsReplicatedEventFunction(const UFunction* Function)
{
	if (!Function) return false;
#if WITH_METADATA
	return Function->HasMetaData(TEXT("CrowdyReplicates"));
#else
	const FCrowdyBakedRpcFunction* Entry = FindRpcFunction(Function);
	return Entry && Entry->bIsReplicated;
#endif
}

bool UCrowdyBakedRegistry::IsPersistentStruct(const UScriptStruct* Struct)
{
	if (!Struct) return false;
#if WITH_METADATA
	return Struct->HasMetaData(TEXT("CrowdyPersistent"));
#else
	const UCrowdyBakedRegistry* Registry = Get();
	if (!Registry) return false;
	Registry->BuildLookups();
	return Registry->PersistentLookup.Contains(FSoftObjectPath(Struct));
#endif
}

bool UCrowdyBakedRegistry::IsSingletonStruct(const UScriptStruct* Struct)
{
	if (!Struct) return false;
#if WITH_METADATA
	return Struct->HasMetaData(TEXT("CrowdySingleton"));
#else
	const UCrowdyBakedRegistry* Registry = Get();
	if (!Registry) return false;
	Registry->BuildLookups();
	return Registry->SingletonLookup.Contains(FSoftObjectPath(Struct));
#endif
}

const FCrowdyBakedRpcFunction* UCrowdyBakedRegistry::FindRpcFunction(const UFunction* Function)
{
	if (!Function) return nullptr;

	const UClass* Owner = Function->GetOwnerClass();
	if (!Owner) return nullptr;

	const UCrowdyBakedRegistry* Registry = Get();
	if (!Registry) return nullptr;

	return Registry->FindRpcFunction(FSoftClassPath(const_cast<UClass*>(Owner)), Function->GetFName());
}

const FCrowdyBakedRpcFunction* UCrowdyBakedRegistry::FindRpcFunction(
	const FSoftClassPath& OwnerClassPath, FName FunctionName) const
{
	BuildLookups();
	const int32* Index = RpcFunctionLookup.Find(TPair<FSoftClassPath, FName>(OwnerClassPath, FunctionName));
	return Index ? &RpcFunctions[*Index] : nullptr;
}

const TArray<FCrowdyBakedRepProperty>* UCrowdyBakedRegistry::FindRepProperties(
	const FSoftClassPath& OwnerClassPath) const
{
	BuildLookups();
	return RepPropertyLookup.Find(OwnerClassPath);
}

int64 UCrowdyBakedRegistry::FindRepLayoutHash(const FSoftClassPath& OwnerClassPath) const
{
	BuildLookups();
	const int64* Hash = RepLayoutHashLookup.Find(OwnerClassPath);
	return Hash ? *Hash : 0;
}

const TArray<FCrowdyBakedRepProperty>* UCrowdyBakedRegistry::FindRepProperties(const UClass* Class)
{
	if (!Class) return nullptr;

	const UCrowdyBakedRegistry* Registry = Get();
	if (!Registry) return nullptr;

	return Registry->FindRepProperties(FSoftClassPath(const_cast<UClass*>(Class)));
}

int64 UCrowdyBakedRegistry::FindRepLayoutHash(const UClass* Class)
{
	if (!Class) return 0;

	const UCrowdyBakedRegistry* Registry = Get();
	if (!Registry) return 0;

	return Registry->FindRepLayoutHash(FSoftClassPath(const_cast<UClass*>(Class)));
}

void UCrowdyBakedRegistry::MakeBakedRepProperties(
	const FCrowdyRepLayout& Layout, const FSoftClassPath& OwnerClassPath, TArray<FCrowdyBakedRepProperty>& OutProps)
{
	OutProps.Reserve(OutProps.Num() + Layout.Properties.Num());
	for (int32 Index = 0; Index < Layout.Properties.Num(); ++Index)
	{
		const FCrowdyRepProperty& Prop = Layout.Properties[Index];

		FCrowdyBakedRepProperty& Baked = OutProps.AddDefaulted_GetRef();
		Baked.OwnerClassPath     = OwnerClassPath;
		Baked.PropertyName       = Prop.Property ? Prop.Property->GetFName() : NAME_None;
		Baked.PropertyID         = Prop.PropertyID;
		Baked.bOwnerOnly         = Prop.bOwnerOnly;
		Baked.bManualDirty       = Prop.bManualDirty;
		Baked.bHeartbeat         = Prop.bHeartbeat;
		Baked.OnRepFunctionName  = Prop.OnRepFunctionName;
		Baked.LayoutOrder        = Index;
	}
}
