// Fill out your copyright notice in the Description page of Project Settings.

#include "GameModel/CrowdyEffectDuplicateFunctionIndex.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Modules/ModuleManager.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/UObjectBaseUtility.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// (container type, function name) -> every Crowdy Effect asset path currently authoring that pair. Unset means
	// "needs a rebuild": the index is built lazily on first use after being invalidated, not eagerly and not on
	// every IsDataValid call, so validating N effects in one pass (a validate-on-save or cook-time sweep) costs one
	// O(A) asset-registry sweep total (A = project's Crowdy Effect asset count) plus O(N) O(1) lookups, not one
	// O(A) sweep per validated asset (O(N*A), quadratic when N ~= A).
	TOptional<TMap<FString, TArray<FString>>> GIndex;

	// container type -> every Game Model function the project's Crowdy Effect assets declare on it. Built by the
	// SAME sweep as GIndex, not a second independent scan of the asset registry: both need to visit every Crowdy
	// Effect asset once, so sweeping apart would double the cost and let the two disagree on which assets they saw.
	// Same "unset means needs a rebuild" discipline as GIndex.
	TOptional<TMap<FString, TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction>>> GFunctionCatalog;

	// Bumped by every invalidation. The sweep captures it before it starts and refuses to publish a map built
	// across an invalidation, because building the index LOADS effect packages and a load is exactly what the
	// invalidators watch: without this, an event raised mid-sweep resets an optional that is not yet set (a no-op)
	// and the already-stale snapshot is then published as current.
	uint32 GIndexGeneration = 0;

	// True only while the sweep below is running. A package load can re-enter validation, and a nested sweep would
	// both recurse and race the outer one's generation check.
	bool GSweepInProgress = false;

	FDelegateHandle GAssetAddedHandle;
	FDelegateHandle GAssetRemovedHandle;
	FDelegateHandle GAssetRenamedHandle;
	FDelegateHandle GPropertyChangedHandle;

	// The index key. Two container types may each declare a function of the same name (the server scopes a model
	// function by its container type), so only the pair identifies a real collision.
	FString MakeDuplicateFunctionIndexKey(const FString& ContainerTypeName, const FString& FunctionName)
	{
		// A line feed appears in neither a container type name nor a function name, so no pair of distinct
		// (type, name) inputs can produce the same key.
		return ContainerTypeName + TEXT("\n") + FunctionName;
	}

	void InvalidateIndex()
	{
		++GIndexGeneration;
		GIndex.Reset();
		GFunctionCatalog.Reset();
	}

	void OnAssetAdded(const FAssetData& AssetData)
	{
		if (AssetData.IsInstanceOf(UCrowdyEffect::StaticClass()))
		{
			InvalidateIndex();
		}
	}

	void OnAssetRemoved(const FAssetData& AssetData)
	{
		if (AssetData.IsInstanceOf(UCrowdyEffect::StaticClass()))
		{
			InvalidateIndex();
		}
	}

	void OnAssetRenamed(const FAssetData& AssetData, const FString& /*OldObjectPath*/)
	{
		if (AssetData.IsInstanceOf(UCrowdyEffect::StaticClass()))
		{
			InvalidateIndex();
		}
	}

	// A live edit (e.g. typing into the FunctionName field) can change what an already-loaded effect's
	// GetEffectiveFunctionName() resolves to, with no asset-registry event of its own until the asset is saved.
	// Invalidate rather than trying to patch one entry: the edit could just as easily be a name a duplicate now
	// collides with, or no longer collides with.
	//
	// The edited object is not always the effect itself. A node in an effect's graph is an editor-only subobject
	// OUTERED to that effect, so editing a node raises this for the NODE, and matching only on the effect would
	// miss every graph edit, including the one that adds the Result node deciding whether the effect returns a
	// value at all. Match on the owning effect too.
	void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& /*Event*/)
	{
		if (!Object)
		{
			return;
		}
		if (Object->IsA<UCrowdyEffect>() || Object->GetTypedOuter<UCrowdyEffect>() != nullptr)
		{
			InvalidateIndex();
		}
	}

	// Builds both GIndex and GFunctionCatalog from one project-wide asset-registry sweep, or leaves them unset when
	// no trustworthy answer is available right now: either a rebuild is already running further up this callstack,
	// or one just raced an invalidation. Returns whether both maps are now set and safe to read. A caller that gets
	// false must report nothing found rather than something possibly stale, so validation never blocks a save (and
	// the Call node picker never offers a callee) on a snapshot known to be out of date; the next call rebuilds and
	// reports it.
	bool BuildIndexesIfNeeded()
	{
		if (GIndex.IsSet() && GFunctionCatalog.IsSet())
		{
			return true;
		}
		if (GSweepInProgress)
		{
			return false;
		}

		TGuardValue<bool> SweepGuard(GSweepInProgress, true);

		FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AR = ARM.Get();

		// Drain the asset registry BEFORE capturing the generation, never after. WaitForCompletion broadcasts the
		// registry's deferred events on this callstack, so a queued asset-added for a Crowdy Effect invalidates
		// right here, before a single asset has been read. Capturing first would record a generation that this
		// drain immediately moves past, and the sweep below would then build a correct result from fully settled
		// state only to throw it away as raced.
		AR.WaitForCompletion();

		const uint32 StartGeneration = GIndexGeneration;

		TMap<FString, TArray<FString>> BuiltIndex;
		TMap<FString, TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction>> BuiltCatalog;

		FARFilter Filter;
		Filter.bRecursiveClasses = true;
		Filter.ClassPaths.Add(UCrowdyEffect::StaticClass()->GetClassPathName());

		TArray<FAssetData> Assets;
		AR.GetAssets(Filter, Assets);

		for (const FAssetData& AssetData : Assets)
		{
			// GetAsset() loads the package if it is not already in memory, which can raise the very events that
			// invalidate these indexes; the generation check below is what keeps such a build from being published.
			if (const UCrowdyEffect* Effect = Cast<UCrowdyEffect>(AssetData.GetAsset()))
			{
				const FString ContainerTypeName = Effect->GetContainerTypeName();
				const FString FunctionName = Effect->GetEffectiveFunctionName();
				const FString AssetPath = AssetData.GetObjectPathString();

				const FString Key = MakeDuplicateFunctionIndexKey(ContainerTypeName, FunctionName);
				BuiltIndex.FindOrAdd(Key).Add(AssetPath);

				if (ContainerTypeName.IsEmpty())
				{
					// The effect has no container class, or its class carries no CrowdyContainer tag. Either way it
					// declares no server function (Compile reports an error and lowers nothing), so it does not
					// belong in the catalog: an empty type name is not a type, and bucketing every such effect under
					// it would offer unrelated broken effects to each other as fn: callees. The duplicate-function
					// index above still records it, because two effects claiming one function name is worth
					// reporting whether or not either of them compiles.
					continue;
				}

				const FCrowdyEffectAuthoredShape Shape = Effect->GetAuthoredShape();

				CrowdyEffectFunctionCatalog::FDeclaredFunction Declared;
				Declared.FunctionName = FunctionName;
				Declared.ContainerTypeName = ContainerTypeName;
				Declared.ReturnType = UCrowdyEffect::ReturnTypeToWireString(Effect->ReturnType);
				Declared.InvokeScope = UCrowdyEffect::CallableFromToWireString(Effect->CallableFrom);
				Declared.bAuthorsReturn = Shape.bAuthorsReturn;
				Declared.bHasMutations = Shape.bHasMutations;
				Declared.AssetPath = AssetPath;
				BuiltCatalog.FindOrAdd(ContainerTypeName).Add(MoveTemp(Declared));
			}
		}

		if (GIndexGeneration != StartGeneration)
		{
			// Something changed while the sweep was running, so what it built is already out of date. Leave both
			// indexes unset and let the next call rebuild from the settled state.
			return false;
		}

		GIndex = MoveTemp(BuiltIndex);
		GFunctionCatalog = MoveTemp(BuiltCatalog);
		return true;
	}

	const TMap<FString, TArray<FString>>* GetOrBuildIndex()
	{
		return BuildIndexesIfNeeded() ? &GIndex.GetValue() : nullptr;
	}

	const TMap<FString, TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction>>* GetOrBuildFunctionCatalog()
	{
		return BuildIndexesIfNeeded() ? &GFunctionCatalog.GetValue() : nullptr;
	}

	TArray<CrowdyEffectDuplicateFunctionCheck::FConflict> FindConflicts(
		const FString& AssetPath, const FString& ContainerTypeName, const FString& FunctionName)
	{
		TArray<CrowdyEffectDuplicateFunctionCheck::FConflict> Out;
		const TMap<FString, TArray<FString>>* Index = GetOrBuildIndex();
		if (!Index)
		{
			return Out;
		}
		if (const TArray<FString>* Paths =
			Index->Find(MakeDuplicateFunctionIndexKey(ContainerTypeName, FunctionName)))
		{
			for (const FString& Path : *Paths)
			{
				if (Path != AssetPath)
				{
					Out.Add({ Path, ContainerTypeName, FunctionName });
				}
			}
		}
		return Out;
	}

	// The declared functions on one container type, or empty when no trustworthy catalog is available right now
	// (see BuildIndexesIfNeeded). Scoped to ContainerTypeName because the server scopes a model function by its
	// container type; the caller (the graph's Call node) is what narrows this further to a single effect's own
	// callable candidates.
	TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction> FindDeclaredFunctions(const FString& ContainerTypeName)
	{
		TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction> Out;
		const TMap<FString, TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction>>* Catalog =
			GetOrBuildFunctionCatalog();
		if (!Catalog)
		{
			return Out;
		}
		if (const TArray<CrowdyEffectFunctionCatalog::FDeclaredFunction>* Functions = Catalog->Find(ContainerTypeName))
		{
			Out = *Functions;
		}
		return Out;
	}
}

void CrowdyEffectDuplicateFunctionIndex::Register()
{
	CrowdyEffectDuplicateFunctionCheck::SetSweepHook(&FindConflicts);
	CrowdyEffectFunctionCatalog::SetCatalogHook(&FindDeclaredFunctions);

	// Reinstalling the hooks above is idempotent, but binding the listeners is not: a second binding would
	// invalidate twice per event and outlive Unregister, which removes one handle each. This handle is the last one
	// registration takes, so a valid one means the whole set is already bound and there is nothing left to do.
	if (GPropertyChangedHandle.IsValid())
	{
		return;
	}

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();
	GAssetAddedHandle = AR.OnAssetAdded().AddStatic(&OnAssetAdded);
	GAssetRemovedHandle = AR.OnAssetRemoved().AddStatic(&OnAssetRemoved);
	GAssetRenamedHandle = AR.OnAssetRenamed().AddStatic(&OnAssetRenamed);
	GPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddStatic(&OnObjectPropertyChanged);
}

void CrowdyEffectDuplicateFunctionIndex::Unregister()
{
	CrowdyEffectDuplicateFunctionCheck::SetSweepHook(nullptr);
	CrowdyEffectFunctionCatalog::SetCatalogHook(nullptr);

	if (FAssetRegistryModule* ARM = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
	{
		IAssetRegistry& AR = ARM->Get();
		AR.OnAssetAdded().Remove(GAssetAddedHandle);
		AR.OnAssetRemoved().Remove(GAssetRemovedHandle);
		AR.OnAssetRenamed().Remove(GAssetRenamedHandle);
	}
	GAssetAddedHandle.Reset();
	GAssetRemovedHandle.Reset();
	GAssetRenamedHandle.Reset();

	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(GPropertyChangedHandle);
	GPropertyChangedHandle.Reset();

	// Bump the generation as well as clearing, so a sweep still unwinding on this callstack cannot publish its
	// result after the module has stopped listening for the events that would keep it current.
	InvalidateIndex();
}

void CrowdyEffectDuplicateFunctionIndex::NotifyEffectAuthoringChanged()
{
	InvalidateIndex();
}
