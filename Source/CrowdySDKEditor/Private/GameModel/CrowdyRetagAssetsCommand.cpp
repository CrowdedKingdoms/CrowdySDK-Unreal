// Fill out your copyright notice in the Description page of Project Settings.

#include "GameModel/CrowdyRetagAssetsCommand.h"

#include "CrowdySDKEditor.h" // LogCrowdyEditor
#include "GameModel/CrowdyContainerScan.h"

#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Replication/GameModel/Effect/CrowdyEffectAuthoredSurface.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h" // FBlueprintTags::ParentClassPath
#include "Engine/Blueprint.h"
#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "Templates/Function.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	IConsoleObject* GRetagAssetsCommand = nullptr;

	// One candidate's raw ParentClassPath tag value, read straight from the registry (never loaded). Empty when the
	// asset declares no parent or the registry has nothing on file for it.
	FString ReadRawParentClassPath(IAssetRegistry& AssetRegistry, const FSoftObjectPath& AssetPath)
	{
		const FAssetData Asset = AssetRegistry.GetAssetByObjectPath(AssetPath, /*bIncludeOnlyOnDiskAssets*/ true);
		FString RawParent;
		Asset.GetTagValue<FString>(FBlueprintTags::ParentClassPath, RawParent);
		return RawParent;
	}

	// A parent named by its generated CLASS object path ("/Game/Foo.Foo_C") back to the Blueprint ASSET path
	// ("/Game/Foo.Foo") the migration's candidate list is keyed by: only a Blueprint's own generated class carries
	// the "_C" suffix on its object name, so a path that does not end in it (a native /Script/ class, or anything
	// malformed) resolves to null and simply never matches a candidate.
	FSoftObjectPath GeneratedClassPathToBlueprintAssetPath(const FString& ClassObjectPath)
	{
		FString PackagePath, ObjectName;
		if (!ClassObjectPath.Split(TEXT("."), &PackagePath, &ObjectName))
		{
			return FSoftObjectPath();
		}
		if (!ObjectName.EndsWith(TEXT("_C")))
		{
			return FSoftObjectPath();
		}
		ObjectName.LeftChopInline(2);
		return FSoftObjectPath(PackagePath + TEXT(".") + ObjectName);
	}

	// One resaved-in-place asset: load, mark dirty, save back to the exact package it came from. Never save-as,
	// never a redirector. Mirrors UCrowdyRegistryBaker::SaveAsset, the plugin's only other asset-resave path; the
	// tag write itself happens inside SavePackage's own GetAssetRegistryTags pass, not here. Deliberately typed on
	// UObject rather than UBlueprint: a Crowdy Effect is a data asset, and it stamps a tag on save the same way a
	// container Blueprint does.
	bool ResaveAsset(const FSoftObjectPath& Path)
	{
		UObject* Asset = Path.TryLoad();
		if (!Asset)
		{
			UE_LOG(LogCrowdyEditor, Warning,
				TEXT("crowdy.schema.RetagAssets: could not load '%s'; skipped."), *Path.ToString());
			return false;
		}

		UPackage* Package = Asset->GetPackage();
		if (!Package)
		{
			UE_LOG(LogCrowdyEditor, Warning,
				TEXT("crowdy.schema.RetagAssets: '%s' has no package; skipped."), *Path.ToString());
			return false;
		}

		Package->MarkPackageDirty();

		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags     = SAVE_NoError;

		const bool bSaved = UPackage::SavePackage(Package, Asset, *FileName, SaveArgs);
		if (!bSaved)
		{
			UE_LOG(LogCrowdyEditor, Warning, TEXT("crowdy.schema.RetagAssets: failed to save '%s'."), *FileName);
		}
		return bSaved;
	}

	// Every Crowdy Effect whose stored authoring surface is missing or was written by a different build. Effects are
	// data assets rather than Blueprints, so the container sweep never sees them, and a plan reads their surface from
	// the same registry it reads container tags from: an effect left untagged is loaded on every plan.
	//
	// No ordering applies here. An effect's payload is a pure function of the effect itself, so unlike a container
	// Blueprint it cannot be invalidated by a sibling being saved afterwards.
	TArray<FSoftObjectPath> GatherUnmigratedEffectAssets(IAssetRegistry& AssetRegistry)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(UCrowdyEffect::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);

		const FName VersionTag(CrowdyEffectTagKeys::AuthoredSurfaceVersion);

		TArray<FSoftObjectPath> Unmigrated;
		for (const FAssetData& Asset : Assets)
		{
			FString Version;
			Asset.GetTagValue<FString>(VersionTag, Version);
			if (!CrowdyEffectAuthoredSurface::IsCurrentTagVersion(Version))
			{
				Unmigrated.Add(Asset.ToSoftObjectPath());
			}
		}

		// The registry's order is not stable between calls, and this list is logged and counted.
		Unmigrated.Sort(FSoftObjectPathLexicalLess());
		return Unmigrated;
	}

	// The full migration: the plan's unmigrated candidates, ordered parent-first, loaded and resaved one at a time.
	// Synchronous and deliberate -- SavePackage has no async form, and this command is a rare, user-invoked, one-time
	// pass rather than something that runs behind ordinary editor use.
	void RunRetagAssets(FOutputDevice& Ar)
	{
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		AssetRegistry.WaitForCompletion();

		const FCrowdyContainerScanPlan Plan = FCrowdyContainerScan::BuildContainerScanPlan();
		const TArray<FSoftObjectPath>& Candidates = Plan.UnmigratedAssets;
		const TArray<FSoftObjectPath> EffectCandidates = GatherUnmigratedEffectAssets(AssetRegistry);

		if (Candidates.IsEmpty() && EffectCandidates.IsEmpty())
		{
			Ar.Logf(TEXT("crowdy.schema.RetagAssets: every scanned asset already carries the current tags; nothing to do."));
			return;
		}

		// Read every candidate's raw parent tag up front: the topo sort needs the whole map before it can order
		// anything, and this is registry-only, so it costs nothing to load.
		TMap<FSoftObjectPath, FSoftObjectPath> ChildToParent;
		ChildToParent.Reserve(Candidates.Num());
		for (const FSoftObjectPath& Candidate : Candidates)
		{
			const FString RawParent = ReadRawParentClassPath(AssetRegistry, Candidate);
			const FString ParentClassPath = CrowdyRetagAssetsCommand::ResolveExportPathToObjectPath(RawParent);
			const FSoftObjectPath ParentAssetPath = GeneratedClassPathToBlueprintAssetPath(ParentClassPath);
			if (!ParentAssetPath.IsNull())
			{
				ChildToParent.Add(Candidate, ParentAssetPath);
			}
		}

		// Containers first, parent-first among themselves, then effects. The two groups are independent: an effect's
		// payload does not read anything a container save could change.
		TArray<FSoftObjectPath> Ordered =
			CrowdyRetagAssetsCommand::TopoSortParentFirst(ChildToParent, Candidates);
		Ordered.Append(EffectCandidates);

		Ar.Logf(TEXT("crowdy.schema.RetagAssets: resaving %d asset(s) to bring their Game Model tags up to date: %d container candidate(s), parent-first, and %d effect(s)."),
			Ordered.Num(), Candidates.Num(), EffectCandidates.Num());

		FScopedSlowTask SlowTask(static_cast<float>(Ordered.Num()),
			FText::FromString(TEXT("Retagging Game Model schema-scan assets...")));
		SlowTask.MakeDialog(/*bShowCancelButton*/ true);

		int32 SavedCount = 0;
		int32 FailedCount = 0;
		int32 VisitedCount = 0;
		for (const FSoftObjectPath& Path : Ordered)
		{
			if (SlowTask.ShouldCancel())
			{
				Ar.Logf(TEXT("crowdy.schema.RetagAssets: cancelled after %d of %d asset(s); re-run the command to finish the rest."),
					VisitedCount, Ordered.Num());
				return;
			}

			SlowTask.EnterProgressFrame(1.0f, FText::FromString(Path.ToString()));

			const bool bSaved = ResaveAsset(Path);
			++VisitedCount;
			if (bSaved)
			{
				++SavedCount;
			}
			else
			{
				++FailedCount;
			}

			Ar.Logf(TEXT("crowdy.schema.RetagAssets: [%d/%d] %s '%s'"),
				VisitedCount, Ordered.Num(), bSaved ? TEXT("resaved") : TEXT("FAILED"), *Path.ToString());
		}

		Ar.Logf(TEXT("crowdy.schema.RetagAssets: done. %d resaved, %d failed, out of %d candidate(s)."),
			SavedCount, FailedCount, Ordered.Num());
	}
}

TArray<FSoftObjectPath> CrowdyRetagAssetsCommand::TopoSortParentFirst(
	const TMap<FSoftObjectPath, FSoftObjectPath>& ChildToParent,
	const TArray<FSoftObjectPath>& Candidates)
{
	TArray<FSoftObjectPath> Ordered;
	Ordered.Reserve(Candidates.Num());

	const TSet<FSoftObjectPath> CandidateSet(Candidates);
	TSet<FSoftObjectPath> Visited;
	TSet<FSoftObjectPath> Visiting; // guards a cycle: reaching a node while its own ancestor walk is still open

	TFunction<void(const FSoftObjectPath&)> Visit = [&](const FSoftObjectPath& Node)
	{
		if (Visited.Contains(Node) || Visiting.Contains(Node))
		{
			// Already placed, or a cycle closing back on a node whose own walk is still open: either way, do not
			// recurse into it again. The cycle case leaves that node to be placed by whichever call opened it.
			return;
		}
		Visiting.Add(Node);

		if (const FSoftObjectPath* Parent = ChildToParent.Find(Node))
		{
			// A parent outside the candidate set (already tag-current, or not a Blueprint at all) needs no resave
			// of its own here, so it is never visited and never appears in the output; the child is simply free to
			// go as soon as its own turn comes.
			if (!Parent->IsNull() && CandidateSet.Contains(*Parent))
			{
				Visit(*Parent);
			}
		}

		Visiting.Remove(Node);
		Visited.Add(Node);
		Ordered.Add(Node);
	};

	for (const FSoftObjectPath& Candidate : Candidates)
	{
		Visit(Candidate);
	}

	return Ordered;
}

FString CrowdyRetagAssetsCommand::ResolveExportPathToObjectPath(const FString& RawExportPathOrNone)
{
	if (RawExportPathOrNone.IsEmpty() || RawExportPathOrNone == TEXT("None"))
	{
		return FString();
	}
	return FPackageName::ExportTextPathToObjectPath(RawExportPathOrNone);
}

void CrowdyRetagAssetsCommand::Register()
{
	if (!GRetagAssetsCommand)
	{
		GRetagAssetsCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("crowdy.schema.RetagAssets"),
			TEXT("One-time migration: resaves every Blueprint the Game Model container scan cannot yet answer for ")
			TEXT("from its asset-registry tags, parent-first, so the scan's fast path covers the whole project. ")
			TEXT("Run on a clean sync with no other outstanding changes; expect version-stamp diff noise."),
			FConsoleCommandWithOutputDeviceDelegate::CreateStatic(&RunRetagAssets),
			ECVF_Default);
	}
}

void CrowdyRetagAssetsCommand::Unregister()
{
	if (GRetagAssetsCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GRetagAssetsCommand);
		GRetagAssetsCommand = nullptr;
	}
}
