// Fill out your copyright notice in the Description page of Project Settings.

#include "GameModel/CrowdyEffectPlanCache.h"

#include "Replication/GameModel/CrowdyCanonicalNumber.h"       // CrowdyCanonicalNumber::ToText
#include "Replication/GameModel/Effect/CrowdyEffect.h"        // UCrowdyEffect
#include "Replication/GameModel/Effect/CrowdyEffectAuthoredSurface.h" // the snapshot both record paths compile
#include "Replication/GameModel/Effect/CrowdyEffectLowering.h" // FCrowdyEffectLoweringResult
#include "Utils/CrowdySDKDeveloperSettings.h"                  // DefaultModelNotificationCarrier

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/StringConv.h"
#include "IO/IoHash.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Templates/Casts.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// A fixed-width digest of a canonical text, so a key stays small however many attributes a container type
	// declares. Hashed over UTF-8 bytes rather than the platform TCHAR, so the digest does not depend on the
	// character width the editor happens to be built with.
	FString HashCanonicalPlanText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		FMD5 Md5;
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, sizeof(Digest));
	}

	// A double as stable text for the vocabulary key. Pinned to the one canonical spelling rather than left to
	// whatever a float print produces, because this key now decides whether a compile recorded on ANOTHER machine
	// still stands: a value that prints two ways would read as a difference that is not there.
	FString ClampBoundToText(double Bound)
	{
		return CrowdyCanonicalNumber::ToText(Bound);
	}
}

FCrowdyEffectPlanCache& FCrowdyEffectPlanCache::Get()
{
	static FCrowdyEffectPlanCache Instance;
	return Instance;
}

void FCrowdyEffectPlanCache::Shutdown()
{
	Get().Clear();
}

FString FCrowdyEffectPlanCache::MakeVocabularyText(
	const FString& TypeName, const TArray<FCrowdyDesiredContainerType>& DesiredTypes)
{
	const FCrowdyDesiredContainerType* Found = DesiredTypes.FindByPredicate(
		[&TypeName](const FCrowdyDesiredContainerType& Candidate)
		{
			return Candidate.TypeName.Equals(TypeName, ESearchCase::CaseSensitive);
		});

	if (!Found)
	{
		// Distinct from a type that exists with no attributes: an effect whose target type has disappeared from the
		// reflected schema must not key the same as one whose target type is merely empty.
		return FString(TEXT("absent\n")) + TypeName;
	}

	TArray<FString> PropLines;
	PropLines.Reserve(Found->Props.Num());
	for (const FCrowdyDesiredPropertyDef& Prop : Found->Props)
	{
		PropLines.Add(FString::Printf(TEXT("%s|%s|%s|%s|%s|%d|%s|%s"),
			*Prop.Key, *Prop.ValueType, *Prop.Visibility, *Prop.Writable, *Prop.DefaultValueJson,
			Prop.bHasClamp ? 1 : 0, *ClampBoundToText(Prop.ClampMin), *ClampBoundToText(Prop.ClampMax)));
	}

	// Sorted case-sensitively so the key is a property of the attribute SET, not of the order reflection happened to
	// return it in. FString's default comparison is case-insensitive, which would leave two keys differing only in
	// case in an order the sort does not fix.
	PropLines.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });

	FString Text = FString(TEXT("type\n")) + Found->TypeName;
	for (const FString& Line : PropLines)
	{
		Text += TEXT("\n");
		Text += Line;
	}
	return Text;
}

FString FCrowdyEffectPlanCache::ComputeVocabularyHash(
	const FString& TypeName, const TArray<FCrowdyDesiredContainerType>& DesiredTypes)
{
	return HashCanonicalPlanText(MakeVocabularyText(TypeName, DesiredTypes));
}

FString FCrowdyEffectPlanCache::ComputeVocabularyHash(
	const FString& TargetTypeName, const FString& SourceTypeName,
	const TArray<FCrowdyDesiredContainerType>& DesiredTypes)
{
	FString Text = MakeVocabularyText(TargetTypeName, DesiredTypes);
	if (!SourceTypeName.IsEmpty())
	{
		// Appended only when a source type is declared, so an effect without one produces the target-only text and
		// keys byte-for-byte as it did before. The section marker keeps the two blocks from running together into a
		// text some other pair of vocabularies could also produce.
		Text += TEXT("\nsource\n");
		Text += MakeVocabularyText(SourceTypeName, DesiredTypes);
	}
	return HashCanonicalPlanText(Text);
}

bool FCrowdyEffectPlanCache::IsRecordReusable(
	const FCrowdyEffectPlanRecord& Record, const FString& PackageHash, const FString& VocabularyHash,
	bool bPackageDirty)
{
	if (bPackageDirty)
	{
		// Unsaved edits are invisible to the saved hash, so a dirty package is always recompiled whatever the
		// hashes say. This is the only rule that can override a full key match.
		return false;
	}
	if (!Record.bReusable)
	{
		return false;
	}
	if (PackageHash.IsEmpty() || Record.PackageHash.IsEmpty())
	{
		// A package the registry has no saved hash for cannot be shown to be unchanged.
		return false;
	}
	if (!Record.PackageHash.Equals(PackageHash, ESearchCase::CaseSensitive))
	{
		return false;
	}
	if (VocabularyHash.IsEmpty() || Record.VocabularyHash.IsEmpty())
	{
		return false;
	}
	return Record.VocabularyHash.Equals(VocabularyHash, ESearchCase::CaseSensitive);
}

bool FCrowdyEffectPlanCache::ShouldStoreRecord(const FCrowdyEffectPlanRecord& Record, bool bPackageDirty)
{
	if (Record.AssetPath.IsEmpty())
	{
		return false;
	}
	if (bPackageDirty)
	{
		// This is a compile of unsaved edits, and the only key there is to file it under is the SAVED package's hash,
		// which describes different content. Reverting or reloading the asset restores that saved content without
		// changing its hash, so the record would then match a key it never described and be served as a valid hit.
		return false;
	}
	// A failed compile, or a target container type that did not resolve, is waiting on something outside both halves
	// of the key: a class that has not been created yet, a CrowdyContainer tag that has not been added, a module that
	// has not loaded. Remembering that answer would keep the effect broken after the thing it needed arrived, so it
	// is recompiled every plan instead.
	return !Record.bCompileFailed && !Record.TargetTypeName.IsEmpty() && !Record.PackageHash.IsEmpty();
}

FString FCrowdyEffectPlanCache::ComputeGlobalSalt()
{
	int32 ProjectCarrier = static_cast<int32>(ECrowdyEffectNotificationCarrier::Channel);
	if (const UCrowdySDKDeveloperSettings* Settings = GetDefault<UCrowdySDKDeveloperSettings>())
	{
		ProjectCarrier = static_cast<int32>(Settings->DefaultModelNotificationCarrier);
	}
	// An effect that defers to the project carrier lowers a different notification when this moves, and no per-asset
	// key can see it, so it invalidates the whole store.
	return FString::Printf(TEXT("carrier=%d"), ProjectCarrier);
}

void FCrowdyEffectPlanCache::BeginPlan(const FString& InSalt)
{
	if (bSaltSet && !Salt.Equals(InSalt, ESearchCase::CaseSensitive))
	{
		Clear();
	}
	Salt = InSalt;
	bSaltSet = true;
}

const FCrowdyEffectPlanRecord* FCrowdyEffectPlanCache::Find(const FString& AssetPath) const
{
	return Records.Find(AssetPath);
}

void FCrowdyEffectPlanCache::Store(const FCrowdyEffectPlanRecord& Record)
{
	if (Record.AssetPath.IsEmpty())
	{
		return;
	}
	Records.Add(Record.AssetPath, Record);
}

void FCrowdyEffectPlanCache::Clear()
{
	Records.Reset();
}

void FCrowdyEffectPlanCache::ProbeEffectAssets(FCrowdyEffectGatherContext& Context)
{
	Context.Probes.Reset();
	Context.bProbed = true;

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	// Instant in a plan: the controller waits for the initial scan asynchronously before it probes, so this only
	// actually blocks for a standalone caller (a test) that reached here with the scan still running.
	AssetRegistry.WaitForCompletion();

	FARFilter Filter;
	Filter.bRecursiveClasses = true;
	Filter.ClassPaths.Add(UCrowdyEffect::StaticClass()->GetClassPathName());

	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	TArray<FName> PackageNames;
	PackageNames.Reserve(Assets.Num());
	for (const FAssetData& Asset : Assets)
	{
		PackageNames.Add(Asset.PackageName);
	}

	// One batched read for the whole project. Each entry is independently optional, so a package the registry knows
	// nothing about leaves its slot unset rather than shifting the ones after it.
	const TArray<TOptional<FAssetPackageData>> PackageData = AssetRegistry.GetAssetPackageDatasCopy(PackageNames);

	const FName SurfaceTag(CrowdyEffectTagKeys::AuthoredSurface);
	const FName SurfaceVersionTag(CrowdyEffectTagKeys::AuthoredSurfaceVersion);

	Context.Probes.Reserve(Assets.Num());
	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		FCrowdyEffectAssetProbe Probe;
		Probe.ObjectPath = Assets[Index].ToSoftObjectPath();
		Probe.AssetPath = Assets[Index].GetObjectPathString();

#if WITH_EDITORONLY_DATA
		if (PackageData.IsValidIndex(Index) && PackageData[Index].IsSet())
		{
			Probe.PackageHash = LexToString(PackageData[Index]->GetPackageSavedHash());
		}
#endif

		// FindPackage never loads: a package that is not already in memory cannot be dirty, so a null answer is the
		// right answer rather than a reason to go looking.
		if (const UPackage* Package = FindPackage(nullptr, *Assets[Index].PackageName.ToString()))
		{
			Probe.bDirty = Package->IsDirty();
		}

		// The surface the asset stamped on itself the last time it was saved. Skipped outright for a dirty package:
		// it describes the SAVED content, and unsaved edits are exactly what a plan of the current project has to
		// see, so a resident dirty asset always goes to the live object instead. A version that is not this build's
		// is treated as no payload at all rather than guessed at.
		if (!Probe.bDirty)
		{
			FString VersionText;
			if (Assets[Index].GetTagValue<FString>(SurfaceVersionTag, VersionText)
				&& CrowdyEffectAuthoredSurface::IsCurrentTagVersion(VersionText))
			{
				FString Payload;
				if (Assets[Index].GetTagValue<FString>(SurfaceTag, Payload) && !Payload.IsEmpty())
				{
					Probe.SurfacePayload = MoveTemp(Payload);
					Probe.bHasSurfaceTag = true;
				}
			}
		}

		if (Context.Cache)
		{
			if (const FCrowdyEffectPlanRecord* Cached = Context.Cache->Find(Probe.AssetPath))
			{
				// Keyed against the types the STORED record compiled against: retargeting, or declaring a different
				// source type, changes the asset itself, so the package hash catches those and this one only has to
				// answer "did either of those types' vocabularies move".
				Probe.VocabularyHash = ComputeVocabularyHash(
					Cached->TargetTypeName, Cached->SourceTypeName, Context.DesiredTypes);
				Probe.bReuseCached = IsRecordReusable(*Cached, Probe.PackageHash, Probe.VocabularyHash, Probe.bDirty);
			}
		}

		Context.Probes.Add(MoveTemp(Probe));
	}

	// Asset-registry sweep order is not stable from one call to the next, and everything downstream inherits it: the
	// order functions are planned in, the order a duplicate-name conflict lists its authors, the order the report
	// reads. Sorting by asset path here makes a plan's output depend on the project rather than on the order a hash
	// map happened to enumerate in, so two plans over unchanged content produce identical text.
	Context.Probes.Sort([](const FCrowdyEffectAssetProbe& A, const FCrowdyEffectAssetProbe& B)
	{
		return A.AssetPath.Compare(B.AssetPath, ESearchCase::CaseSensitive) < 0;
	});
}

FCrowdyEffectPlanRecord FCrowdyEffectPlanCache::CompileRecord(
	const UCrowdyEffect& Effect, const FCrowdyEffectAssetProbe& Probe,
	const TArray<FCrowdyDesiredContainerType>& DesiredTypes)
{
	TArray<FCrowdyEffectDiagnostic> SurfaceDiagnostics;
	const FCrowdyEffectAuthoredSurface Surface = CrowdyEffectAuthoredSurface::FromEffect(Effect, SurfaceDiagnostics);
	return BuildRecordFromSurface(Surface, SurfaceDiagnostics, Probe, DesiredTypes);
}

FCrowdyEffectPlanRecord FCrowdyEffectPlanCache::BuildRecordFromSurface(
	const FCrowdyEffectAuthoredSurface& Surface, const TArray<FCrowdyEffectDiagnostic>& SurfaceDiagnostics,
	const FCrowdyEffectAssetProbe& Probe, const TArray<FCrowdyDesiredContainerType>& DesiredTypes)
{
	FCrowdyEffectPlanRecord Record;
	Record.AssetPath = Probe.AssetPath;
	Record.PackageHash = Probe.PackageHash;
	Record.EffectiveFunctionName = Surface.EffectiveFunctionName;
	// Empty for an effect whose source shares the target's type, which is what keeps such an effect's key unchanged.
	Record.SourceTypeName = Surface.SourceContainerType;

	{
		// The record keeps only Error diagnostics, and the fn-callee catalog contributes warnings alone, so
		// compiling without it produces an identical record. What it buys is enormous on a real project: a catalog
		// cache miss synchronously force-loads EVERY Crowdy Effect asset mid-lowering, on the game thread, which
		// bypasses the plan's async streaming entirely and was most of the plan's remaining hitch.
		//
		// The target type comes back from the same call rather than being resolved separately, so it is recorded
		// even for an effect that will be passed over (a skipped effect is still reported against the model it
		// belongs to) and it can never disagree with the type the compile validated against.
		const FCrowdyEffectLoweringResult Result = CrowdyEffectAuthoredSurface::CompileResolved(
			Surface, ECrowdyEffectFnCatalog::None, SurfaceDiagnostics, Record.TargetTypeName);
		if (Result.HasErrors())
		{
			Record.bCompileFailed = true;
			Record.FirstCompileError = TEXT("a compile error");
			for (const FCrowdyEffectDiagnostic& Diagnostic : Result.Diagnostics)
			{
				if (Diagnostic.Severity == ECrowdyEffectSeverity::Error)
				{
					Record.FirstCompileError = Diagnostic.ToString();
					break;
				}
			}
		}
		else
		{
			Record.Function = Result.Function;
			if (Result.Automation.IsSet())
			{
				Record.bHasAutomation = true;
				Record.Automation = Result.Automation.GetValue();
			}
			if (Result.Trigger.IsSet())
			{
				Record.bHasTrigger = true;
				Record.Trigger = Result.Trigger.GetValue();
			}
		}
	}

	Record.VocabularyHash = ComputeVocabularyHash(Record.TargetTypeName, Record.SourceTypeName, DesiredTypes);
	Record.bReusable = ShouldStoreRecord(Record, Probe.bDirty);
	return Record;
}

const TArray<FCrowdyEffectPlanRecord>& FCrowdyEffectPlanCache::BuildRecords(FCrowdyEffectGatherContext& Context)
{
	if (Context.bRecordsBuilt)
	{
		return Context.Records;
	}
	if (!Context.bProbed)
	{
		ProbeEffectAssets(Context);
	}

	Context.Records.Reset();
	Context.Records.Reserve(Context.Probes.Num());

	// Always empty: this rung's surface came off disk, and a surface that did not build is never written there.
	const TArray<FCrowdyEffectDiagnostic> NoSurfaceDiagnostics;

	for (const FCrowdyEffectAssetProbe& Probe : Context.Probes)
	{
		if (Probe.bReuseCached && Context.Cache)
		{
			if (const FCrowdyEffectPlanRecord* Cached = Context.Cache->Find(Probe.AssetPath))
			{
				Context.Records.Add(*Cached);
				continue;
			}
		}

		if (Probe.bHasSurfaceTag)
		{
			FCrowdyEffectAuthoredSurface Surface;
			if (CrowdyEffectAuthoredSurface::FromJson(Probe.SurfacePayload, Surface))
			{
				FCrowdyEffectPlanRecord Record =
					BuildRecordFromSurface(Surface, NoSurfaceDiagnostics, Probe, Context.DesiredTypes);
				if (Context.Cache && ShouldStoreRecord(Record, Probe.bDirty))
				{
					Context.Cache->Store(Record);
				}
				Context.Records.Add(MoveTemp(Record));
				continue;
			}
			// A payload this build cannot read says nothing about the effect, so it is passed over in favour of the
			// asset itself rather than treated as an effect that authors nothing.
		}

		// Resolve rather than load: a caller that streamed its misses in first finds every one of these resident, so
		// this is an in-memory lookup. TryLoad is the fallback for a caller that streamed nothing (a test, or a plan
		// whose stream could not run), which keeps the standalone behaviour a synchronous force-load.
		UObject* Object = Probe.ObjectPath.ResolveObject();
		if (!Object)
		{
			Object = Probe.ObjectPath.TryLoad();
		}

		const UCrowdyEffect* Effect = Cast<UCrowdyEffect>(Object);
		if (!Effect)
		{
			// A registry entry that does not resolve to an effect is passed over silently, exactly as the gathers
			// have always done: it contributes nothing and is not an authoring mistake to report.
			continue;
		}

		FCrowdyEffectPlanRecord Record = CompileRecord(*Effect, Probe, Context.DesiredTypes);
		if (Context.Cache && ShouldStoreRecord(Record, Probe.bDirty))
		{
			Context.Cache->Store(Record);
		}
		Context.Records.Add(MoveTemp(Record));
	}

	Context.bRecordsBuilt = true;
	return Context.Records;
}
