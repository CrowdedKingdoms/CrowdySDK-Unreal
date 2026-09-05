// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CrowdyStudioModule.h"
#include "Engine/Blueprint.h"
#include "GameModel/CrowdyContainerScan.h"
#include "GameModel/CrowdySchemaSync.h"
#include "HAL/PlatformTime.h"
#include "Modules/ModuleManager.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "UObject/Class.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyContainerScanTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Named apart from the other fixtures in this module because adaptive unity merges its translation units and
	// two anonymous-namespace helpers sharing a name redefine each other.
	FCrowdyScannedAsset MakeScannedAsset(const TCHAR* ObjectPath, const TCHAR* ScanVersion, const TCHAR* ContainerType)
	{
		FCrowdyScannedAsset Asset;
		Asset.ObjectPath = FSoftObjectPath(ObjectPath);
		Asset.AssetPath = ObjectPath;
		Asset.ScanVersion = ScanVersion;
		Asset.ContainerTypeName = ContainerType;
		return Asset;
	}

	bool CrowdyContainerScanPlanLoads(const FCrowdyContainerScanPlan& Plan, const TCHAR* ObjectPath)
	{
		return Plan.AssetsToLoad.Contains(FSoftObjectPath(ObjectPath));
	}

	// Two reflected container types compared the whole way down, attributes included. A count-only comparison would
	// pass a schema whose defaults or value types moved, which is exactly what a plan then writes to a live app.
	bool CrowdyContainerScanTypesMatch(const FCrowdyDesiredContainerType& A, const FCrowdyDesiredContainerType& B)
	{
		const auto Same = [](const FString& X, const FString& Y) { return X.Equals(Y, ESearchCase::CaseSensitive); };

		if (!Same(A.TypeName, B.TypeName)
			|| !Same(A.DisplayName, B.DisplayName)
			|| !Same(A.InstantiableBy, B.InstantiableBy)
			|| !Same(A.DefaultVisibility, B.DefaultVisibility)
			|| !Same(A.OwningClassPath, B.OwningClassPath)
			|| A.Props.Num() != B.Props.Num())
		{
			return false;
		}

		for (int32 Index = 0; Index < A.Props.Num(); ++Index)
		{
			const FCrowdyDesiredPropertyDef& P = A.Props[Index];
			const FCrowdyDesiredPropertyDef& Q = B.Props[Index];
			if (P.PropertyName != Q.PropertyName
				|| !Same(P.Key, Q.Key) || !Same(P.ValueType, Q.ValueType) || !Same(P.DefaultValueJson, Q.DefaultValueJson)
				|| !Same(P.Visibility, Q.Visibility) || !Same(P.Writable, Q.Writable)
				|| P.bHasClamp != Q.bHasClamp || P.ClampMin != Q.ClampMin || P.ClampMax != Q.ClampMax)
			{
				return false;
			}
		}
		return true;
	}

	// Reflection order follows the class iterator, which is not stable from one call to the next, so both sides are
	// put in a canonical order before they are compared at all.
	void SortDesiredTypesByName(TArray<FCrowdyDesiredContainerType>& Types)
	{
		Types.Sort([](const FCrowdyDesiredContainerType& A, const FCrowdyDesiredContainerType& B)
		{
			return A.TypeName.Compare(B.TypeName, ESearchCase::CaseSensitive) < 0;
		});
	}
}

// The scan-version rule on its own: an asset the registry has never described, or described under an older format,
// has to be opened. This is also what keeps an UNTAGGED container visible, since from the registry an untagged
// container and an untagged anything-else are the same thing, and the only safe reading of "I do not know" is to go
// and look. A container that quietly vanished from a plan would make every model the live app holds a prune
// candidate, which is the worst thing this whole mechanism could do.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerScanVersionPartitionTest,
	"CrowdySDK.CrowdyStudio.ContainerScanPartitionsOnTheScanVersion", CrowdyContainerScanTestFlags)
bool FCrowdyContainerScanVersionPartitionTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyScannedAsset> Assets;
	Assets.Add(MakeScannedAsset(TEXT("/Game/NeverDescribed.NeverDescribed"), TEXT(""), TEXT("")));
	Assets.Add(MakeScannedAsset(TEXT("/Game/OlderFormat.OlderFormat"), TEXT("0"), TEXT("")));

	const FCrowdyContainerScanPlan Plan = FCrowdyContainerScan::PartitionScannedAssets(Assets);

	TestTrue(TEXT("an asset the registry never described is opened"),
		CrowdyContainerScanPlanLoads(Plan, TEXT("/Game/NeverDescribed.NeverDescribed")));
	TestTrue(TEXT("an asset described under an older format is opened"),
		CrowdyContainerScanPlanLoads(Plan, TEXT("/Game/OlderFormat.OlderFormat")));
	TestEqual(TEXT("both count as not yet tag-migrated, which is what the status line reports"),
		Plan.UnmigratedAssetCount, 2);
	// The counters are what makes the two rules distinguishable: neither of these assets was ANSWERED, so neither
	// may be filed as a container or as a non-container.
	TestEqual(TEXT("an undescribed asset is never counted as a known container"), Plan.KnownContainerCount, 0);
	TestEqual(TEXT("an undescribed asset is never counted as a known non-container"), Plan.KnownNonContainerCount, 0);
	TestEqual(TEXT("both undescribed assets are in the retag migration's candidate set"),
		Plan.UnmigratedAssets.Num(), 2);
	TestTrue(TEXT("the never-described asset is in the migration candidate set"),
		Plan.UnmigratedAssets.Contains(FSoftObjectPath(TEXT("/Game/NeverDescribed.NeverDescribed"))));
	TestTrue(TEXT("the older-format asset is in the migration candidate set"),
		Plan.UnmigratedAssets.Contains(FSoftObjectPath(TEXT("/Game/OlderFormat.OlderFormat"))));

	return true;
}

// The container-tag rule on its own, over assets the registry HAS described. This is the negative marker doing its
// job: without it every non-container would stay in the load partition forever and the sweep would never shrink.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerScanTagPartitionTest,
	"CrowdySDK.CrowdyStudio.ContainerScanReadsContainerHoodFromTheTag", CrowdyContainerScanTestFlags)
bool FCrowdyContainerScanTagPartitionTest::RunTest(const FString& Parameters)
{
	const TCHAR* CurrentVersion = CrowdyGameModelMetaKeys::ScanAssetTagValue;

	TArray<FCrowdyScannedAsset> Assets;
	Assets.Add(MakeScannedAsset(TEXT("/Game/Chest.Chest"), CurrentVersion, TEXT("Chest")));
	Assets.Add(MakeScannedAsset(TEXT("/Game/Widget.Widget"), CurrentVersion, TEXT("")));

	const FCrowdyContainerScanPlan Plan = FCrowdyContainerScan::PartitionScannedAssets(Assets);

	// A container still loads: the schema is reflected from the live class, and the tag only decided that the
	// project did not have to open everything else to find it.
	TestTrue(TEXT("a described container is still opened, since the schema comes from the live class"),
		CrowdyContainerScanPlanLoads(Plan, TEXT("/Game/Chest.Chest")));
	TestFalse(TEXT("a described non-container is not opened at all"),
		CrowdyContainerScanPlanLoads(Plan, TEXT("/Game/Widget.Widget")));
	TestEqual(TEXT("one described container"), Plan.KnownContainerCount, 1);
	TestEqual(TEXT("one described non-container"), Plan.KnownNonContainerCount, 1);
	TestEqual(TEXT("nothing described needs migrating"), Plan.UnmigratedAssetCount, 0);
	if (TestEqual(TEXT("the container's type name is reported"), Plan.KnownContainerTypeNames.Num(), 1))
	{
		TestEqual(TEXT("and it is the name the tag carried"),
			Plan.KnownContainerTypeNames[0], FString(TEXT("Chest")));
	}
	// The retag command reads UnmigratedAssets, not AssetsToLoad, to decide what to resave: an already-current
	// container has nothing left to gain from another resave, so it must stay out of that list even though the
	// scan still opens it for its schema.
	TestTrue(TEXT("an already-current known container is not in the retag migration's candidate set"),
		Plan.UnmigratedAssets.IsEmpty());

	return true;
}

// Native containers, which no tag can ever describe: a native class has no asset, so it has no registry entry to
// carry one. This is a required rung of the scan rather than a fallback, and this project really has one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerScanNativePathTest,
	"CrowdySDK.CrowdyStudio.ContainerScanFindsNativeContainers", CrowdyContainerScanTestFlags)
bool FCrowdyContainerScanNativePathTest::RunTest(const FString& Parameters)
{
	const TArray<UClass*> NativeContainers = FCrowdySchemaSync::GatherNativeContainerClasses();
	const TArray<UClass*> AllContainers = FCrowdySchemaSync::GatherContainerClasses();

	for (const UClass* Class : NativeContainers)
	{
		UE_LOG(LogCrowdyStudio, Display, TEXT("Native Game Model container: %s"),
			Class ? *Class->GetPathName() : TEXT("<null>"));
	}

	TestTrue(TEXT("the project declares at least one native container"), NativeContainers.Num() > 0);

	bool bAllNative = true;
	bool bAllReachTheSchema = true;
	for (UClass* Class : NativeContainers)
	{
		bAllNative = bAllNative && Class && Class->HasAnyClassFlags(CLASS_Native);
		// The union is the point: a native container that the native gather finds but the schema gather does not
		// would be discovered and then dropped, which is the same outcome as never finding it.
		bAllReachTheSchema = bAllReachTheSchema && AllContainers.Contains(Class);
	}
	TestTrue(TEXT("every class the native gather returns is native"), bAllNative);
	TestTrue(TEXT("every native container also reaches the schema gather"), bAllReachTheSchema);

	// The specific one this project ships. Named by path rather than by C++ type so this module does not have to
	// depend on the plugin that declares it.
	if (UClass* TitanAttributes = FindObject<UClass>(
		nullptr, TEXT("/Script/CKTitanAssault.CKTitanAssaultGameModelAttributeComponent")))
	{
		TestTrue(TEXT("the Titan Assault attribute component is found as a native container"),
			NativeContainers.Contains(TitanAttributes));
	}
	else
	{
		UE_LOG(LogCrowdyStudio, Display,
			TEXT("Container scan: CKTitanAssault is not loaded here, so its native container could not be checked by name."));
	}

	return true;
}

// The exclusion rule behind the scanned-roots setting. An empty entry is ignored on purpose: honouring one would
// turn a stray blank line in a config file into "scan nothing", which reads on the server as a project that
// declares no containers at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerScanRootMatchTest,
	"CrowdySDK.CrowdyStudio.ContainerScanRootMatching", CrowdyContainerScanTestFlags)
bool FCrowdyContainerScanRootMatchTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Roots = { TEXT("/Game/Vendor"), TEXT("/Skelot/") };

	TestTrue(TEXT("a root matches itself"), FCrowdyContainerScan::IsPathUnderAnyRoot(TEXT("/Game/Vendor"), Roots));
	TestTrue(TEXT("a path under a root matches"),
		FCrowdyContainerScan::IsPathUnderAnyRoot(TEXT("/Game/Vendor/Meshes"), Roots));
	TestTrue(TEXT("a trailing slash on the root is ignored"),
		FCrowdyContainerScan::IsPathUnderAnyRoot(TEXT("/Skelot/Anims"), Roots));
	TestTrue(TEXT("matching ignores case, since these are typed by hand"),
		FCrowdyContainerScan::IsPathUnderAnyRoot(TEXT("/game/vendor"), Roots));
	// The trap this guards: a sibling whose name merely starts with the same characters is a different root.
	TestFalse(TEXT("a sibling root sharing a name prefix does not match"),
		FCrowdyContainerScan::IsPathUnderAnyRoot(TEXT("/Game/VendorTools"), Roots));
	TestFalse(TEXT("an unrelated path does not match"),
		FCrowdyContainerScan::IsPathUnderAnyRoot(TEXT("/Game/Player"), Roots));
	TestFalse(TEXT("an empty root never matches"),
		FCrowdyContainerScan::IsPathUnderAnyRoot(TEXT("/Game/Player"), TArray<FString>({ FString(), TEXT("  ") })));

	return true;
}

// The characterization gate: the schema the partitioned scan produces over THIS project, against the schema the
// sweep it replaced produces over the same roots. The partitioned pass runs FIRST, so it cannot borrow anything the
// full sweep opened; if the two agree afterwards, nothing was lost by not opening the rest.
//
// This is also where "an untagged container is still found" is really answered. Until the project is re-saved, no
// asset carries a scan tag at all, so every container here is an untagged one and the whole schema comes out of the
// load partition.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerScanMatchesFullSweepTest,
	"CrowdySDK.CrowdyStudio.ContainerScanMatchesTheLoadEverythingSweep", CrowdyContainerScanTestFlags)
bool FCrowdyContainerScanMatchesFullSweepTest::RunTest(const FString& Parameters)
{
	const double ScanStart = FPlatformTime::Seconds();
	const FCrowdyContainerScanPlan Plan = FCrowdyContainerScan::BuildContainerScanPlan();

	// Resolved synchronously rather than through StreamContainerScanPlan: the streamed form completes on a later
	// frame, and what is being gated here is the PARTITION, not the streaming.
	for (const FSoftObjectPath& Path : Plan.AssetsToLoad)
	{
		if (!Path.ResolveObject())
		{
			Path.TryLoad();
		}
	}

	TArray<FString> ScanWarnings;
	TArray<FCrowdyDesiredContainerType> FromScan =
		FCrowdySchemaSync::BuildDesiredSchema(FCrowdySchemaSync::GatherContainerClasses(), ScanWarnings);
	const double ScanMs = (FPlatformTime::Seconds() - ScanStart) * 1000.0;

	// The comparison side: every Blueprint in the same roots, opened, exactly as the scan's predecessor did.
	const double SweepStart = FPlatformTime::Seconds();
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	AssetRegistry.WaitForCompletion();

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	for (const FString& Root : FCrowdyContainerScan::ResolveScannedRoots())
	{
		Filter.PackagePaths.AddUnique(FName(*Root));
	}

	TArray<FAssetData> SweptAssets;
	if (Filter.PackagePaths.Num() > 0)
	{
		AssetRegistry.GetAssets(Filter, SweptAssets);
	}
	for (const FAssetData& Asset : SweptAssets)
	{
		Asset.GetAsset();
	}

	TArray<FString> SweepWarnings;
	TArray<FCrowdyDesiredContainerType> FromFullSweep =
		FCrowdySchemaSync::BuildDesiredSchema(FCrowdySchemaSync::GatherContainerClasses(), SweepWarnings);
	const double SweepMs = (FPlatformTime::Seconds() - SweepStart) * 1000.0;

	UE_LOG(LogCrowdyStudio, Display,
		TEXT("Container scan: %s Partitioned pass %.1f ms; the load-everything sweep it replaced opened %d asset(s) in %.1f ms. "
		     "%d container type(s) from the scan, %d from the sweep."),
		*Plan.Describe(), ScanMs, SweptAssets.Num(), SweepMs, FromScan.Num(), FromFullSweep.Num());

	SortDesiredTypesByName(FromScan);
	SortDesiredTypesByName(FromFullSweep);

	if (!TestEqual(TEXT("the partitioned scan reflects the same number of container types as the full sweep"),
		FromScan.Num(), FromFullSweep.Num()))
	{
		for (const FCrowdyDesiredContainerType& Type : FromFullSweep)
		{
			UE_LOG(LogCrowdyStudio, Display, TEXT("Full sweep container type: %s (%s)"),
				*Type.TypeName, *Type.OwningClassPath);
		}
		return false;
	}

	bool bIdentical = true;
	for (int32 Index = 0; Index < FromFullSweep.Num(); ++Index)
	{
		if (!CrowdyContainerScanTypesMatch(FromScan[Index], FromFullSweep[Index]))
		{
			UE_LOG(LogCrowdyStudio, Display,
				TEXT("Container scan: type '%s' differs between the partitioned scan and the full sweep."),
				*FromFullSweep[Index].TypeName);
			bIdentical = false;
		}
	}
	TestTrue(TEXT("the partitioned scan produces the same schema as the sweep it replaced, field for field"),
		bIdentical);

	// The plan's own accounting has to add up, or the number the status line shows a user is made up.
	TestEqual(TEXT("everything the plan loads is either a named container or an unanswered asset"),
		Plan.AssetsToLoad.Num(), Plan.KnownContainerCount + Plan.UnmigratedAssetCount);
	TestEqual(TEXT("every swept asset was accounted for exactly once"),
		Plan.SweptAssetCount, Plan.KnownContainerCount + Plan.KnownNonContainerCount + Plan.UnmigratedAssetCount);

	return true;
}

#endif
