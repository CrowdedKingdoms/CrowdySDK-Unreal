// Fill out your copyright notice in the Description page of Project Settings.

#include "Baking/CrowdyRegistryBaker.h"

#include "CrowdySDKEditor.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Utils/CrowdyBakedRegistry.h"
#include "Utils/CrowdySDKDeveloperSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Interfaces/IPluginManager.h"
#include "PluginDescriptor.h"
#include "PluginReferenceDescriptor.h"
#include "GameDelegates.h"
#include "Engine/Blueprint.h"
#include "StructUtils/UserDefinedStruct.h"
#include "ToolMenus.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

#define LOCTEXT_NAMESPACE "CrowdyRegistryBaker"

namespace
{
	bool IsTransientClassName(const FString& Name)
	{
		return Name.StartsWith(TEXT("SKEL_"))
			|| Name.StartsWith(TEXT("REINST_"))
			|| Name.StartsWith(TEXT("TRASHCLASS_"))
			|| Name.StartsWith(TEXT("PLACEHOLDER-"));
	}

	const TCHAR* GSdkPluginName = TEXT("CrowdySDK");

	// Async (button) rebuild state: one at a time. The handle keeps the streamed assets resident
	// until the bake runs in its completion callback.
	bool GAsyncRebuildInFlight = false;
	TSharedPtr<FStreamableHandle> GAsyncRebuildHandle;

	// Snapshots a CrowdyEvent function's routing/identity for the baked asset.
	// BuildFnInfo reads the same live metadata + reflection the runtime uses in the
	// editor, so the baked values match what the cooked runtime would compute.
	FCrowdyBakedRpcFunction MakeBakedRpcEntry(const FSoftClassPath& ClassPath, UFunction* Function)
	{
		const FCrowdyFnInfo Info = FCrowdyRPC::BuildFnInfo(Function);

		FCrowdyBakedRpcFunction Entry;
		Entry.ClassPath    = ClassPath;
		Entry.FunctionName = Function->GetFName();
		Entry.FunctionID   = Info.FunctionID;
		Entry.Recipient    = Info.Recipient;
		Entry.DecayRate    = Info.DecayRate;
		Entry.Distance     = Info.Distance;
		Entry.ChannelName  = Info.ChannelName;
		Entry.bParamsPOD   = Info.bParamsPOD;
		Entry.bIsReplicated = CrowdyRpcMetaKeys::HasReplicatesMeta(Function);
		return Entry;
	}

	// True if Plugin's transitive .uplugin dependency closure references CrowdySDK.
	// Lets a consumer plugin's content be scanned no matter where it's installed
	// (project or engine), without dragging in unrelated engine sample content.
	bool PluginDependsOnSDK(const TSharedRef<IPlugin>& Plugin, IPluginManager& PM, TSet<FString>& Visited)
	{
		if (Visited.Contains(Plugin->GetName())) return false;
		Visited.Add(Plugin->GetName());

		for (const FPluginReferenceDescriptor& Ref : Plugin->GetDescriptor().Plugins)
		{
			if (Ref.Name == GSdkPluginName) return true;
			if (const TSharedPtr<IPlugin> Dep = PM.FindPlugin(Ref.Name))
			{
				if (PluginDependsOnSDK(Dep.ToSharedRef(), PM, Visited))
					return true;
			}
		}
		return false;
	}
}

//Triggers

void UCrowdyRegistryBaker::Register()
{
	// The trigger that makes this fully automatic: regenerate the registry at the
	// start of every cook and inject it into the cook set. Runs in the cook's
	// editor process, where metadata still exists. No user action, ever.
	FGameDelegates::Get().GetModifyCookDelegate().AddStatic(&UCrowdyRegistryBaker::OnModifyCook);

	// ToolMenus is not ready at module-startup time; defer until it is.
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateStatic(&UCrowdyRegistryBaker::OnStartup));
}

void UCrowdyRegistryBaker::OnModifyCook(
	TConstArrayView<const ITargetPlatform*> /*TargetPlatforms*/,
	TArray<FName>& PackagesToCook,
	TArray<FName>& /*PackagesToNeverCook*/)
{
	// Deep so assets never opened this session are captured; the cook loads
	// everything anyway.
	Rebuild(/*bDeep*/true);

	// Guarantee the freshly-baked asset is in the build, independent of whether
	// any soft reference happens to pull it in.
	PackagesToCook.AddUnique(FName(CrowdyBakedRegistryPaths::PackagePath));

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdyRegistryBaker] Cook hook: baked + injected '%s'."),
		CrowdyBakedRegistryPaths::PackagePath);
}

void UCrowdyRegistryBaker::OnStartup()
{
	if (UToolMenus* ToolMenus = UToolMenus::Get())
	{
		UToolMenu* Menu = ToolMenus->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
		FToolMenuSection& Section =
			Menu->FindOrAddSection(TEXT("CrowdySDK"), LOCTEXT("CrowdySDKSection", "Crowdy SDK"));

		Section.AddMenuEntry(
			TEXT("RebuildCrowdyRegistry"),
			LOCTEXT("RebuildCrowdyRegistry", "Rebuild Crowdy Registry"),
			LOCTEXT("RebuildCrowdyRegistryTip",
				"Optional. Manually re-bakes the Crowdy metadata registry now. Packaging "
				"does this automatically at cook time, so you normally never need this — "
				"it's here for inspecting the baked asset in-editor."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([] { UCrowdyRegistryBaker::Rebuild(/*bDeep*/true); })));
	}

	// First-run convenience: a project with no asset yet gets a populated one, so
	// it exists in-editor. Loaded-scope to avoid a startup hitch the cook hook
	// does the authoritative deep bake, so completeness here doesn't matter.
	if (!ResolveAsset() && !LoadObject<UCrowdyBakedRegistry>(nullptr, CrowdyBakedRegistryPaths::ObjectPath))
		Rebuild(/*bDeep*/false);
}

// Rebuild

void UCrowdyRegistryBaker::Rebuild(bool bDeep)
{
	if (bDeep)
		LoadAllTaggedAssets();

	FinishRebuild(nullptr);
}

void UCrowdyRegistryBaker::RebuildAsync(TFunction<void()> OnComplete)
{
	// No interactive editor to keep responsive (cook / commandlet) just do the synchronous bake.
	if (IsRunningCommandlet())
	{
		Rebuild(/*bDeep*/ true);
		if (OnComplete) OnComplete();
		return;
	}

	if (GAsyncRebuildInFlight)
	{
		UE_LOG(LogCrowdyEditor, Log,
			TEXT("[CrowdyRegistryBaker] Async rebuild already in progress , ignoring re-request."));
		if (OnComplete) OnComplete();
		return;
	}

	// Query is cheap (no loading); we only stream the assets the query returns.
	TArray<FAssetData> Assets;
	GatherTaggedAssets(Assets);

	TArray<FSoftObjectPath> Paths;
	Paths.Reserve(Assets.Num());
	for (const FAssetData& Asset : Assets)
		Paths.Add(Asset.ToSoftObjectPath());

	if (Paths.Num() == 0)
	{
		// Nothing to stream only already-loaded objects to sweep, which is cheap; finish inline.
		FinishRebuild(MoveTemp(OnComplete));
		return;
	}

	GAsyncRebuildInFlight = true;

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdyRegistryBaker] Async deep rebuild: streaming %d tagged asset(s) without blocking the editor…"),
		Paths.Num());

	// The force-load was the hitch. Stream the assets in instead; when they are resident the
	// completion delegate (game thread) runs the same sweep + save the synchronous path does.
	GAsyncRebuildHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
		MoveTemp(Paths),
		FStreamableDelegate::CreateLambda([OnComplete = MoveTemp(OnComplete)]() mutable
		{
			FinishRebuild(MoveTemp(OnComplete));
			GAsyncRebuildHandle.Reset();
			GAsyncRebuildInFlight = false;
		}),
		FStreamableManager::AsyncLoadHighPriority);
}

void UCrowdyRegistryBaker::FinishRebuild(TFunction<void()> OnComplete)
{
	UCrowdyBakedRegistry* Registry = ResolveOrCreateAsset();
	if (!Registry)
	{
		UE_LOG(LogCrowdyEditor, Warning,
			TEXT("[CrowdyRegistryBaker] Could not resolve or create the baked registry asset."));
		if (OnComplete) OnComplete();
		return;
	}

	PopulateFromLoadedObjects(Registry);

	// During a cook the cooker holds the existing .uasset open, so SavePackage's
	// in-place replace fails ("Error moving file ... .tmp"). We don't need to
	// write: the cooker serializes the in-memory object we just populated, and we
	// inject the package via PackagesToCook in OnModifyCook. Still write when
	// interactive, or when no file exists yet (no open-file conflict, and gives
	// the cook something to load).
	const bool bExistsOnDisk =
		FPackageName::DoesPackageExist(FString(CrowdyBakedRegistryPaths::PackagePath));
	if (!IsRunningCommandlet() || !bExistsOnDisk)
		SaveAsset(Registry);

	// Editor/PIE reads live metadata, but drop any stale cache for good measure.
	UCrowdyBakedRegistry::InvalidateCache();

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdyRegistryBaker] Baked %d persistent + %d singleton struct(s), "
		     "%d RPC function(s), %d rep prop(s) across %d class layout(s)."),
		Registry->PersistentStructs.Num(), Registry->SingletonStructs.Num(),
		Registry->RpcFunctions.Num(),
		Registry->RepProperties.Num(), Registry->RepLayoutHashes.Num());

	if (OnComplete) OnComplete();
}

void UCrowdyRegistryBaker::UpdateForClass(UClass* Class)
{
	if (!Class) return;

	// During a cook, OnModifyCook already performs the full authoritative bake.
	// Blueprints compiled-on-load by the cooker would otherwise each call ResolveAsset
	// here, LoadObject-ing the baked registry recursively while another package is mid-load
	// the engine logs deadlock-avoidance partial loads and UnexpectedLoad warnings for it.
	// The per-class refresh is purely for in-editor inspection, so skip it under the cooker.
	if (IsRunningCookCommandlet()) return;

	// Don't create the asset from a compile , startup / the menu action own
	// creation. If it doesn't exist yet, the next full bake captures this class.
	UCrowdyBakedRegistry* Registry = ResolveAsset();
	if (!Registry) return;

	if (IsTransientClassName(Class->GetName())) return;

	const FSoftClassPath Path(Class);

	Registry->RpcFunctions.RemoveAll(
		[&Path](const FCrowdyBakedRpcFunction& Entry) { return Entry.ClassPath == Path; });

	for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (!It->HasMetaData(CrowdyMetaKeys::CrowdyEvent)) continue;
		Registry->RpcFunctions.Add(MakeBakedRpcEntry(Path, *It));
	}

	// Same evict-and-refill for this class's CrowdyState rep layout. In-memory inspection only, so
	// like the RPC refill above we don't re-sort the arrays.
	Registry->RepProperties.RemoveAll(
		[&Path](const FCrowdyBakedRepProperty& Entry) { return Entry.OwnerClassPath == Path; });
	Registry->RepLayoutHashes.RemoveAll(
		[&Path](const FCrowdyBakedRepLayoutHash& Entry) { return Entry.ClassPath == Path; });

	FCrowdyRepLayout Layout;
	if (FCrowdyStateLayoutBuilder::BuildLayout(Class, Layout))
	{
		UCrowdyBakedRegistry::MakeBakedRepProperties(Layout, Path, Registry->RepProperties);
		Registry->RepLayoutHashes.Add({ Path, Layout.LayoutHash });
	}

	// In-memory refresh only, so the asset reflects this class if someone opens it
	// in-editor this session. We deliberately do NOT MarkPackageDirty(): the baked
	// registry is a derived cook artifact (regenerated in full by OnModifyCook) that
	// is never read in editor/PIE (those use live metadata), so dirtying it here did
	// nothing but force a needless manual save on every Blueprint compile.
}

// Scanning

void UCrowdyRegistryBaker::PopulateFromLoadedObjects(UCrowdyBakedRegistry* Registry)
{
	if (!Registry) return;

	Registry->PersistentStructs.Reset();
	Registry->SingletonStructs.Reset();
	Registry->RpcFunctions.Reset();
	Registry->RepProperties.Reset();
	Registry->RepLayoutHashes.Reset();

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (IsTransientClassName(Class->GetName())) continue;

		const FSoftClassPath Path(Class);

		for (TFieldIterator<UFunction> F(Class, EFieldIteratorFlags::ExcludeSuper); F; ++F)
		{
			if (!F->HasMetaData(CrowdyMetaKeys::CrowdyEvent)) continue;
			Registry->RpcFunctions.Add(MakeBakedRpcEntry(Path, *F));
		}
	}

	// CrowdyState rep layouts, a separate pass over classes so it stays independent of the RPC
	// loop above. BuildLayout walks the whole class (IncludeSuper), so unlike the ExcludeSuper RPC
	// scan every class is inspected here and MakeBakedRepProperties records the positional order.
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (IsTransientClassName(Class->GetName())) continue;

		FCrowdyRepLayout Layout;
		if (!FCrowdyStateLayoutBuilder::BuildLayout(Class, Layout)) continue;

		const FSoftClassPath Path(Class);
		UCrowdyBakedRegistry::MakeBakedRepProperties(Layout, Path, Registry->RepProperties);
		Registry->RepLayoutHashes.Add({ Path, Layout.LayoutHash });
	}

	for (TObjectIterator<UScriptStruct> It; It; ++It)
	{
		UScriptStruct* Struct = *It;
		const FSoftObjectPath Path(Struct);

		if (Struct->HasMetaData(CrowdyMetaKeys::CrowdyPersistent))
			Registry->PersistentStructs.AddUnique(Path);
		if (Struct->HasMetaData(CrowdyMetaKeys::CrowdySingleton))
			Registry->SingletonStructs.AddUnique(Path);
	}

	// TObjectIterator order is not stable across runs or machines, so sort every
	// array into a canonical order before the asset is saved/cooked. This makes the
	// baked bytes deterministic , identical metadata always yields an identical
	// asset , so two machines never produce a spurious binary diff, and the cooked
	// output is reproducible.
	auto ByPath = [](const FSoftObjectPath& A, const FSoftObjectPath& B)
	{
		return A.ToString() < B.ToString();
	};
	Registry->PersistentStructs.Sort(ByPath);
	Registry->SingletonStructs.Sort(ByPath);
	Registry->RpcFunctions.Sort([](const FCrowdyBakedRpcFunction& A, const FCrowdyBakedRpcFunction& B)
	{
		const FString AClass = A.ClassPath.ToString();
		const FString BClass = B.ClassPath.ToString();
		return AClass != BClass ? AClass < BClass : A.FunctionName.ToString() < B.FunctionName.ToString();
	});

	// Same canonical ordering for the rep arrays: group properties by class, then by their positional
	// LayoutOrder so the baked order matches the live declaration order the codec addresses by index.
	Registry->RepProperties.Sort([](const FCrowdyBakedRepProperty& A, const FCrowdyBakedRepProperty& B)
	{
		const FString AClass = A.OwnerClassPath.ToString();
		const FString BClass = B.OwnerClassPath.ToString();
		return AClass != BClass ? AClass < BClass : A.LayoutOrder < B.LayoutOrder;
	});
	Registry->RepLayoutHashes.Sort([](const FCrowdyBakedRepLayoutHash& A, const FCrowdyBakedRepLayoutHash& B)
	{
		return A.ClassPath.ToString() < B.ClassPath.ToString();
	});
}

void UCrowdyRegistryBaker::GatherTaggedAssets(TArray<FAssetData>& OutAssets)
{
	FAssetRegistryModule& ARM =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();
	AR.WaitForCompletion();

	// CRITICAL: scope the scan to the project's own content. An unscoped scan
	// force-loads every Blueprint in the engine + all plugins to read editor-only
	// metadata; when this runs from the cook's ModifyCook hook, those loads are
	// seen by the cooker as unsolicited and dragged into the build , including
	// engine sample content that hard-references NeverCook editor assets
	// (EditorCube, TargetIcon), which fails the cook. Only /Game and project-type
	// plugins (CrowdySDK, CK*) can declare Crowdy metadata, so restrict to those.
	FARFilter Filter;
	Filter.bRecursivePaths   = true;
	Filter.bRecursiveClasses = true;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.ClassPaths.Add(UUserDefinedStruct::StaticClass()->GetClassPathName());

	Filter.PackagePaths.Add(FName(TEXT("/Game")));

	IPluginManager& PM = IPluginManager::Get();
	for (const TSharedRef<IPlugin>& Plugin : PM.GetEnabledPluginsWithContent())
	{
		// Include a plugin's content if it is the SDK, is user content (project /
		// mod plugins are scanned anyway and cooked anyway), or depends on the SDK
		// (a consumer plugin built on CrowdySDK, wherever it is installed). Engine
		// and unrelated third-party plugins , where the NeverCook sample content
		// lives , are skipped.
		const EPluginType Type = Plugin->GetType();
		const bool bIsUserContent = (Type == EPluginType::Project || Type == EPluginType::Mod);
		const bool bIsSdk         = (Plugin->GetName() == GSdkPluginName);

		bool bConsumesSdk = false;
		if (!bIsUserContent && !bIsSdk)
		{
			TSet<FString> Visited;
			bConsumesSdk = PluginDependsOnSDK(Plugin, PM, Visited);
		}

		if (!bIsUserContent && !bIsSdk && !bConsumesSdk)
			continue;

		FString Mount = Plugin->GetMountedAssetPath(); // e.g. "/CrowdySDK/"
		Mount.RemoveFromEnd(TEXT("/"));
		if (!Mount.IsEmpty())
			Filter.PackagePaths.AddUnique(FName(*Mount));
	}

	AR.GetAssets(Filter, OutAssets);
}

void UCrowdyRegistryBaker::LoadAllTaggedAssets()
{
	// Deep/cook path: force everything resident now. A cook must have the loads done before it
	// serializes the bake, so this stays synchronous; the interactive button streams instead
	// (see RebuildAsync) to avoid freezing the editor.
	TArray<FAssetData> Assets;
	GatherTaggedAssets(Assets);
	for (const FAssetData& Asset : Assets)
		Asset.GetAsset();
}

// ─── Asset plumbing ─────────────────────────────────────────────────────────

UCrowdyBakedRegistry* UCrowdyRegistryBaker::ResolveAsset()
{
	// Optional explicit override, then the fixed path. LoadObject pulls the
	// existing on-disk package into memory so the cook modifies the real instance.
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
		if (UCrowdyBakedRegistry* FromSettings = Settings->BakedRegistry.LoadSynchronous())
			return FromSettings;

	return LoadObject<UCrowdyBakedRegistry>(nullptr, CrowdyBakedRegistryPaths::ObjectPath);
}

UCrowdyBakedRegistry* UCrowdyRegistryBaker::ResolveOrCreateAsset()
{
	if (UCrowdyBakedRegistry* Existing = ResolveAsset())
	{
		// Deleting the asset in-editor strips its RF_Public/RF_Standalone flags, but our root
		// (UCrowdyBakedRegistry::Get's AddToRoot) keeps the now-flagless object alive at its original
		// path , so ResolveAsset hands the deleted object straight back. SavePackage then refuses to
		// write it ("does not have any of the provided object flags … saving would cause data loss"),
		// and the rebuild silently fails to persist. Re-assert the asset flags and re-register it so a
		// rebuild-after-delete recreates the .uasset on disk instead.
		if (!Existing->HasAllFlags(RF_Public | RF_Standalone))
		{
			Existing->SetFlags(RF_Public | RF_Standalone);
			Existing->ClearFlags(RF_Transient);
			FAssetRegistryModule::AssetCreated(Existing);
			Existing->MarkPackageDirty();

			UE_LOG(LogCrowdyEditor, Log,
				TEXT("[CrowdyRegistryBaker] Re-established the baked registry asset after an in-editor delete; it will be recreated on save."));
		}
		return Existing;
	}

	const FString PackageName = CrowdyBakedRegistryPaths::PackagePath;
	const FString AssetName   = TEXT("CrowdyBakedRegistry");

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package) return nullptr;
	Package->FullyLoad();

	UCrowdyBakedRegistry* Asset = NewObject<UCrowdyBakedRegistry>(
		Package, *AssetName, RF_Public | RF_Standalone);
	if (!Asset) return nullptr;

	FAssetRegistryModule::AssetCreated(Asset);
	Asset->MarkPackageDirty();

	// We deliberately do not write the settings soft-pointer to DefaultGame.ini here.
	// The runtime resolves the asset by its fixed path when the pointer is unset (see
	// ResolveAsset / UCrowdyBakedRegistry::Get), so the auto-write only churned a
	// version-controlled config file and differed per machine. The BakedRegistry
	// setting still works as an optional manual override if a project sets it.

	UE_LOG(LogCrowdyEditor, Log,
		TEXT("[CrowdyRegistryBaker] Created baked registry asset at '%s'."), *PackageName);

	return Asset;
}

void UCrowdyRegistryBaker::SaveAsset(UCrowdyBakedRegistry* Registry)
{
	if (!Registry) return;

	UPackage* Package = Registry->GetPackage();
	if (!Package) return;
	Package->MarkPackageDirty();

	const FString FileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags     = SAVE_NoError;

	if (!UPackage::SavePackage(Package, Registry, *FileName, SaveArgs))
	{
		UE_LOG(LogCrowdyEditor, Warning,
			TEXT("[CrowdyRegistryBaker] Failed to save baked registry to '%s'."), *FileName);
	}
}

#undef LOCTEXT_NAMESPACE
