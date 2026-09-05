// Fill out your copyright notice in the Description page of Project Settings.

#include "Utils/CrowdyBakedRegistry.h"
#include "CrowdyReplicationLog.h"
#include "Utils/CrowdySDKDeveloperSettings.h"
#include "Replication/State/FCrowdyRepLayout.h" // FCrowdyRepLayout, FCrowdyRepProperty
#include "Replication/GameModel/CrowdyAttributeRegistry.h" // FCrowdyAttributeDef
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h" // CrowdyPullOnStart
#include "UObject/Class.h"          // UFunction, UClass
#include "UObject/UObjectGlobals.h"

namespace
{
	// Cached singleton. The asset is referenced by the developer settings soft
	// pointer, so it cooks; we keep our own root reference so a GC pass between
	// load and use cannot collect it.
	TWeakObjectPtr<const UCrowdyBakedRegistry> GCachedRegistry;
	bool bCacheResolved = false;

#if WITH_METADATA
	// The pull-on-start tag is an opt-out: the only value that turns the pull off is "False", so anything else a
	// class carries (and the absence of the tag entirely) means it pulls. Compared without case sensitivity so a
	// hand-written meta=(CrowdyPullOnStart="false") reads the same as the value the Blueprint compiler stamps.
	bool PullOnStartValueMeansPull(const FString& Value)
	{
		return !Value.TrimStartAndEnd().Equals(TEXT("False"), ESearchCase::IgnoreCase);
	}
#endif
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

	// Group the flat model-attribute list by owning class (order within a class is not addressed by
	// index the way rep layout is, so no defensive re-sort is needed).
	for (const FCrowdyBakedAttribute& Attr : ModelAttributes)
	{
		ModelAttributeLookup.FindOrAdd(Attr.OwnerClassPath).Add(Attr);
	}

	ModelClassLookup.Reserve(ModelClasses.Num());
	for (const FCrowdyBakedModelClass& Entry : ModelClasses)
	{
		ModelClassLookup.Add(Entry.ClassPath, Entry);
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

const TArray<FCrowdyBakedAttribute>* UCrowdyBakedRegistry::FindModelAttributes(
	const FSoftClassPath& OwnerClassPath) const
{
	BuildLookups();
	return ModelAttributeLookup.Find(OwnerClassPath);
}

const TArray<FCrowdyBakedAttribute>* UCrowdyBakedRegistry::FindModelAttributes(const UClass* Class)
{
	if (!Class) return nullptr;

	const UCrowdyBakedRegistry* Registry = Get();
	if (!Registry) return nullptr;

	return Registry->FindModelAttributes(FSoftClassPath(const_cast<UClass*>(Class)));
}

bool UCrowdyBakedRegistry::FindContainerTypeName(const FSoftClassPath& ClassPath, FString& OutTypeName) const
{
	BuildLookups();
	if (const FCrowdyBakedModelClass* Entry = ModelClassLookup.Find(ClassPath))
	{
		if (!Entry->ContainerTypeName.IsEmpty())
		{
			OutTypeName = Entry->ContainerTypeName;
			return true;
		}
	}
	return false;
}

bool UCrowdyBakedRegistry::FindContainerTypeName(const UClass* Class, FString& OutTypeName)
{
	if (!Class) return false;

	const UCrowdyBakedRegistry* Registry = Get();
	if (!Registry) return false;

	return Registry->FindContainerTypeName(FSoftClassPath(const_cast<UClass*>(Class)), OutTypeName);
}

bool UCrowdyBakedRegistry::FindPullModelOnStart(const FSoftClassPath& ClassPath, bool& bOutPullOnStart) const
{
	BuildLookups();
	if (const FCrowdyBakedModelClass* Entry = ModelClassLookup.Find(ClassPath))
	{
		bOutPullOnStart = Entry->bPullOnStart;
		return true;
	}
	return false;
}

bool UCrowdyBakedRegistry::ShouldPullModelOnStart(const UClass* Class)
{
	if (!Class)
	{
		return true;
	}

#if WITH_METADATA
	// Metadata lookups answer for one class, so a subclass that declares no tag of its own follows the nearest
	// ancestor that does. The first class in the chain carrying the tag decides, which lets a subclass declare
	// "True" to opt back in to the pull its base turned off.
	for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
	{
		if (Current->HasMetaData(CrowdyGameModelMetaKeys::PullOnStart))
		{
			return PullOnStartValueMeansPull(Current->GetMetaData(CrowdyGameModelMetaKeys::PullOnStart));
		}
	}
	return true;
#else
	// Cooked builds strip UCLASS metadata. The bake already resolved the chain walk above into each container
	// class's own entry, so the nearest ancestor with an entry carries this class's answer.
	const UCrowdyBakedRegistry* Registry = Get();
	if (!Registry)
	{
		return true;
	}
	for (const UClass* Current = Class; Current; Current = Current->GetSuperClass())
	{
		bool bPullOnStart = true;
		if (Registry->FindPullModelOnStart(FSoftClassPath(const_cast<UClass*>(Current)), bPullOnStart))
		{
			return bPullOnStart;
		}
	}
	return true;
#endif
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

void UCrowdyBakedRegistry::MakeBakedAttributes(
	const TArray<FCrowdyAttributeDef>& Defs, const FSoftClassPath& OwnerClassPath, TArray<FCrowdyBakedAttribute>& OutAttrs)
{
	OutAttrs.Reserve(OutAttrs.Num() + Defs.Num());
	for (const FCrowdyAttributeDef& Def : Defs)
	{
		FCrowdyBakedAttribute& Baked = OutAttrs.AddDefaulted_GetRef();
		Baked.OwnerClassPath    = OwnerClassPath;
		Baked.PropertyName      = Def.PropertyName;
		Baked.Key               = Def.Key;
		Baked.ValueType         = Def.ValueType;
		Baked.bHasClamp         = Def.bHasClamp;
		Baked.ClampMin          = Def.ClampMin;
		Baked.ClampMax          = Def.ClampMax;
		Baked.OnRepFunctionName = Def.OnRepFunctionName;
	}
}
