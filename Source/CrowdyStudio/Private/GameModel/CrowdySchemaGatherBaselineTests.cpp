// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CrowdyStudioModule.h"
#include "GameModel/CrowdyEffectPlanCache.h"
#include "GameModel/CrowdySchemaSync.h"
#include "HAL/PlatformTime.h"
#include "Modules/ModuleManager.h"
#include "Replication/GameModel/Effect/CrowdyEffect.h"
#include "Templates/Casts.h"

namespace
{
	constexpr EAutomationTestFlags CrowdySchemaGatherBaselineTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Two planned functions compared the whole way down, elements included: a count-only comparison of the parameters,
	// mutations, notifications and timers would pass a function whose writes target the wrong attribute, which is
	// exactly the difference worth catching. Named apart from the other fixtures in this module because adaptive unity
	// merges its translation units and two anonymous-namespace helpers sharing a name redefine each other.
	bool CrowdyBaselineFunctionsMatch(
		const FCrowdyGameModelFunctionInput& A, const FCrowdyGameModelFunctionInput& B)
	{
		const auto Same = [](const FString& X, const FString& Y) { return X.Equals(Y, ESearchCase::CaseSensitive); };

		if (!Same(A.Name, B.Name)
			|| !Same(A.ContainerTypeName, B.ContainerTypeName)
			|| !Same(A.Description, B.Description)
			|| !Same(A.InvokeScope, B.InvokeScope)
			|| !Same(A.InvokePolicyJson, B.InvokePolicyJson)
			|| !Same(A.ReturnType, B.ReturnType)
			|| !Same(A.ReturnExpression, B.ReturnExpression)
			|| A.bAutonomousInvocable != B.bAutonomousInvocable
			|| A.Parameters.Num() != B.Parameters.Num()
			|| A.Mutations.Num() != B.Mutations.Num()
			|| A.Notifications.Num() != B.Notifications.Num()
			|| A.Timers.Num() != B.Timers.Num())
		{
			return false;
		}

		for (int32 Index = 0; Index < A.Parameters.Num(); ++Index)
		{
			const FCrowdyGameModelFunctionParam& P = A.Parameters[Index];
			const FCrowdyGameModelFunctionParam& Q = B.Parameters[Index];
			if (!Same(P.Name, Q.Name) || !Same(P.ValueType, Q.ValueType) || !Same(P.DefaultValueJson, Q.DefaultValueJson)
				|| !Same(P.Description, Q.Description) || P.bRequired != Q.bRequired || P.SortOrder != Q.SortOrder)
			{
				return false;
			}
		}

		for (int32 Index = 0; Index < A.Mutations.Num(); ++Index)
		{
			const FCrowdyGameModelMutation& M = A.Mutations[Index];
			const FCrowdyGameModelMutation& N = B.Mutations[Index];
			if (!Same(M.Target, N.Target) || !Same(M.Property, N.Property) || !Same(M.Expression, N.Expression))
			{
				return false;
			}
		}

		for (int32 Index = 0; Index < A.Notifications.Num(); ++Index)
		{
			const FCrowdyGameModelNotification& M = A.Notifications[Index];
			const FCrowdyGameModelNotification& N = B.Notifications[Index];
			if (!Same(M.Kind, N.Kind) || !Same(M.EmitAs, N.EmitAs) || M.Args.Num() != N.Args.Num())
			{
				return false;
			}
			for (int32 ArgIndex = 0; ArgIndex < M.Args.Num(); ++ArgIndex)
			{
				if (!Same(M.Args[ArgIndex].Name, N.Args[ArgIndex].Name)
					|| !Same(M.Args[ArgIndex].Expression, N.Args[ArgIndex].Expression))
				{
					return false;
				}
			}
		}

		for (int32 Index = 0; Index < A.Timers.Num(); ++Index)
		{
			const FCrowdyGameModelTimer& T = A.Timers[Index];
			const FCrowdyGameModelTimer& U = B.Timers[Index];
			if (!Same(T.FunctionName, U.FunctionName) || !Same(T.Target, U.Target)
				|| !Same(T.DelayMsExpression, U.DelayMsExpression)
				|| !Same(T.DedupeKeyExpression, U.DedupeKeyExpression)
				|| T.Params.Num() != U.Params.Num())
			{
				return false;
			}
			for (int32 ParamIndex = 0; ParamIndex < T.Params.Num(); ++ParamIndex)
			{
				if (!Same(T.Params[ParamIndex].Name, U.Params[ParamIndex].Name)
					|| !Same(T.Params[ParamIndex].Expression, U.Params[ParamIndex].Expression))
				{
					return false;
				}
			}
		}

		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySchemaGatherDesiredFunctionsBaselineTest,
	"CrowdySDK.CrowdyStudio.GatherDesiredFunctionsBaseline", CrowdySchemaGatherBaselineTestFlags)

bool FCrowdySchemaGatherDesiredFunctionsBaselineTest::RunTest(const FString& Parameters)
{
	// Nothing is whitelisted as an expected error on purpose. The gather reports every effect-level complaint
	// (unmigrated, non-compiling, empty or reserved name, duplicate name) through its warnings array and never
	// through the log, so a pattern aimed at those matches nothing. The one error this test can realistically
	// provoke is an asset that fails to load while every effect is force-loaded, and that is worth failing on:
	// a pattern broad enough to cover it would also hide unrelated failures raised while this test runs.

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	AssetRegistry.WaitForCompletion();

	FARFilter Filter;
	Filter.bRecursiveClasses = true;
	Filter.ClassPaths.Add(UCrowdyEffect::StaticClass()->GetClassPathName());

	TArray<FAssetData> EffectAssets;
	AssetRegistry.GetAssets(Filter, EffectAssets);

	TArray<FString> Warnings;
	TSet<FString> RecognizedNames;
	TArray<FCrowdySchemaAuthorship> Authorship;

	// No gather context: this measures the standalone path, which sweeps and loads every effect and remembers
	// nothing. Handing it the plan cache would measure whatever a previous test left behind instead.
	const double StartSeconds = FPlatformTime::Seconds();
	const TArray<FCrowdyGameModelFunctionInput> Functions =
		FCrowdySchemaSync::GatherDesiredFunctions(Warnings, RecognizedNames, Authorship);
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

	// The measured cost of the synchronous, load-everything gather. Compare a later run against this line to show
	// that async streaming plus the content-hash skip actually moved the number; the cost scales with the
	// effect-asset count, so the three figures only mean anything together.
	UE_LOG(LogCrowdyStudio, Display,
		TEXT("GatherDesiredFunctions baseline: %.1f ms, %d function(s) returned, %d UCrowdyEffect asset(s) in the project."),
		ElapsedMs, Functions.Num(), EffectAssets.Num());

	// The same resolution the gather does: a registry entry that does not resolve to a UCrowdyEffect is passed over
	// silently by it, so it must not be counted here either. Every asset is already resident by now, so this pass is
	// a lookup rather than a second load and cannot distort the figure above.
	int32 ResolvedEffectCount = 0;
	for (const FAssetData& AssetData : EffectAssets)
	{
		if (Cast<UCrowdyEffect>(AssetData.GetAsset()))
		{
			++ResolvedEffectCount;
		}
	}

	// The warnings decide which of the checks below apply, so a red run is only diagnosable with them on the log.
	for (const FString& Warning : Warnings)
	{
		UE_LOG(LogCrowdyStudio, Display, TEXT("GatherDesiredFunctions baseline warning: %s"), *Warning);
	}

	// No timing assert on purpose: a wall-clock threshold would fail on a slow or loaded machine without telling
	// anyone anything true. The line above is the record; what is worth asserting is the shape of what came back.
	TestTrue(TEXT("The gather returns at most one function per effect asset"),
		Functions.Num() <= ResolvedEffectCount);

	// Authorship covers every asset the gather visited, skipped ones included, which is the whole point of it: a name
	// that never reached the server still has to be traceable back to the asset that claims it.
	TestEqual(TEXT("One authorship entry per effect asset, skipped ones included"),
		Authorship.Num(), ResolvedEffectCount);

	bool bEveryFunctionNamed = true;
	bool bEveryNameRecognized = true;
	bool bEveryScopedNameUnique = true;
	TSet<FString> SeenScopedNames;
	SeenScopedNames.Reserve(Functions.Num());
	for (const FCrowdyGameModelFunctionInput& Function : Functions)
	{
		bEveryFunctionNamed = bEveryFunctionNamed && !Function.Name.IsEmpty();
		bEveryNameRecognized = bEveryNameRecognized && RecognizedNames.Contains(Function.Name);

		bool bAlreadySeen = false;
		SeenScopedNames.Add(FCrowdySchemaSync::ScopedNameKey(Function.ContainerTypeName, Function.Name), &bAlreadySeen);
		bEveryScopedNameUnique = bEveryScopedNameUnique && !bAlreadySeen;
	}

	TestTrue(TEXT("Every gathered function carries a name, since a nameless one is skipped and warned about"),
		bEveryFunctionNamed);
	// The recognized set is what keeps a live server function off the prune list, so a gathered function missing
	// from it would be planned as an orphan while its own upsert is planned alongside.
	TestTrue(TEXT("Every gathered function's name was also recorded as recognized"), bEveryNameRecognized);
	// Identity is the (container type, name) pair, and a pair claimed by two effects is excluded for both authors,
	// so the same pair can never appear twice. The same name on two different container types is not a duplicate.
	TestTrue(TEXT("No container type and function name pair is gathered twice"), bEveryScopedNameUnique);

	// The check that a silently empty gather cannot pass. Every path that drops an effect from the desired list
	// also records a warning, so a warning-free gather over a project that has effects must return one function per
	// effect. Returning fewer means functions vanished without being reported, which makes the schema sync plan the
	// live server function of each one as an orphan to prune. Two states legitimately skip this: a project with no
	// effect assets at all (a fresh project), and a project whose warnings account for the missing functions.
	if (ResolvedEffectCount > 0 && Warnings.Num() == 0)
	{
		TestEqual(TEXT("A warning-free gather returns one function per effect asset"),
			Functions.Num(), ResolvedEffectCount);
	}
	else
	{
		UE_LOG(LogCrowdyStudio, Display,
			TEXT("GatherDesiredFunctions baseline: the one-function-per-effect check does not apply here (%d effect asset(s) resolved, %d warning(s))."),
			ResolvedEffectCount, Warnings.Num());
	}

	return true;
}

// What a plan built entirely out of stored results contains, compared against a plan built with the cache taken out of
// the picture altogether. A reused record is served verbatim and sent to the server field for field, so anything the
// store rounds off or hands back from the wrong asset ships to a live app and is invisible until something breaks in
// play.
//
// What this does NOT gate, deliberately: whether the cache's KEY is strict enough. Nothing about the project moves
// between these passes, so a key that ignored the package hash, the vocabulary or the dirty flag would still hand back
// the right answer here. Each of those rules is gated on its own, against a moved key, in CrowdyEffectPlanCacheTests.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEffectPlanCacheSecondPassMatchesTest,
	"CrowdySDK.CrowdyStudio.EffectPlanCacheSecondPassMatchesAFreshCompile", CrowdySchemaGatherBaselineTestFlags)

bool FCrowdyEffectPlanCacheSecondPassMatchesTest::RunTest(const FString& Parameters)
{
	// A cache of this test's own, so neither the editor session's store nor a later plan is disturbed by it.
	FCrowdyEffectPlanCache Cache;
	Cache.BeginPlan(FCrowdyEffectPlanCache::ComputeGlobalSalt());

	TArray<FString> SchemaWarnings;
	// Timed because it is the one part of a plan that stays on the game thread by nature: it is live reflection over
	// loaded classes, so there is nothing to stream and nothing to cache. If it were ever the expensive step, no
	// amount of async around it would help, and it would be worth knowing before reaching for one.
	const double ReflectStart = FPlatformTime::Seconds();
	const TArray<FCrowdyDesiredContainerType> DesiredTypes =
		FCrowdySchemaSync::BuildDesiredSchema(FCrowdySchemaSync::GatherContainerClasses(), SchemaWarnings);
	const double ReflectMs = (FPlatformTime::Seconds() - ReflectStart) * 1000.0;

	// A null cache is not the same thing as an empty one: it disables storing as well as reuse, which is what makes a
	// pass run with it a compile the cache had no part in at all.
	auto RunOnePass = [&DesiredTypes](FCrowdyEffectPlanCache* PassCache,
		TArray<FCrowdyGameModelFunctionInput>& OutFunctions, int32& OutAssetsNeedingLoad, double& OutElapsedMs)
	{
		FCrowdyEffectGatherContext Context;
		Context.DesiredTypes = DesiredTypes;
		Context.Cache = PassCache;

		const double Start = FPlatformTime::Seconds();
		FCrowdyEffectPlanCache::ProbeEffectAssets(Context);

		OutAssetsNeedingLoad = 0;
		for (const FCrowdyEffectAssetProbe& Probe : Context.Probes)
		{
			if (Probe.NeedsLoad())
			{
				++OutAssetsNeedingLoad;
			}
		}

		TArray<FString> Warnings;
		TSet<FString> RecognizedNames;
		TArray<FCrowdySchemaAuthorship> Authorship;
		OutFunctions = FCrowdySchemaSync::GatherDesiredFunctions(Warnings, RecognizedNames, Authorship, &Context);
		OutElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	};

	TArray<FCrowdyGameModelFunctionInput> FirstFunctions;
	int32 FirstPassLoads = 0;
	double FirstPassMs = 0.0;
	RunOnePass(&Cache, FirstFunctions, FirstPassLoads, FirstPassMs);

	const int32 StoredAfterFirstPass = Cache.Num();

	TArray<FCrowdyGameModelFunctionInput> CachedFunctions;
	int32 CachedPassLoads = 0;
	double CachedPassMs = 0.0;
	RunOnePass(&Cache, CachedFunctions, CachedPassLoads, CachedPassMs);

	// The comparison side: the same project planned again with no cache at all, so every effect is compiled from the
	// asset itself. Without this pass the only thing the cached results could be checked against is the run that
	// produced them, which is a value against a copy of itself and green whatever the store does.
	TArray<FCrowdyGameModelFunctionInput> FreshFunctions;
	int32 FreshPassLoads = 0;
	double FreshPassMs = 0.0;
	RunOnePass(nullptr, FreshFunctions, FreshPassLoads, FreshPassMs);

	UE_LOG(LogCrowdyStudio, Display,
		TEXT("Effect plan cache: first pass %.1f ms over %d asset(s) needing a load, cached pass %.1f ms over %d, "
		     "cache-free pass %.1f ms over %d, %d record(s) stored. Class reflection %.1f ms."),
		FirstPassMs, FirstPassLoads, CachedPassMs, CachedPassLoads, FreshPassMs, FreshPassLoads, StoredAfterFirstPass,
		ReflectMs);

	// The split that decides whether the compile is worth chunking across frames. The first pass loads AND compiles;
	// the cache-free pass compiles the same assets with every one of them already resident, so it is the compile on
	// its own. In production the effect assets are streamed before either gather runs, which means the cache-free
	// figure, not the first-pass one, is what the game thread actually spends.
	if (FirstPassLoads > 0)
	{
		UE_LOG(LogCrowdyStudio, Display,
			TEXT("Plan cost split over %d effect(s): loading %.1f ms, compiling %.1f ms (%.1f%% of it is loading). "
			     "Per effect: %.2f ms to load, %.3f ms to compile."),
			FirstPassLoads, FirstPassMs - FreshPassMs, FreshPassMs,
			FirstPassMs > 0.0 ? 100.0 * (FirstPassMs - FreshPassMs) / FirstPassMs : 0.0,
			(FirstPassMs - FreshPassMs) / FirstPassLoads, FreshPassMs / FirstPassLoads);
	}

	// Every effect the first pass could key is skipped entirely by the second: that is the whole point of the cache,
	// and it is a counted fact rather than a wall-clock one, so it means the same thing on any machine.
	// Clamped at zero because a pass can now need no loads at all: an effect that carries its authored surface in
	// the asset registry is read without opening the package, so the first pass can store records for effects it
	// never loaded. Subtracting a stored count from a load count of zero is not a smaller number of loads, it is a
	// meaningless one.
	TestEqual(TEXT("The cached pass needs to load only what the first pass could not store"),
		CachedPassLoads, FMath::Max(0, FirstPassLoads - StoredAfterFirstPass));
	// And the cache-free pass compiles everything, which is what makes it the comparison side rather than a third
	// reader of the same stored answers.
	TestEqual(TEXT("The cache-free pass compiles every effect the project has"), FreshPassLoads, FirstPassLoads);

	if (!TestEqual(TEXT("Both plans contain the same number of functions"),
		CachedFunctions.Num(), FreshFunctions.Num()))
	{
		return false;
	}

	bool bIdentical = true;
	for (int32 Index = 0; Index < FreshFunctions.Num(); ++Index)
	{
		if (!CrowdyBaselineFunctionsMatch(FreshFunctions[Index], CachedFunctions[Index]))
		{
			UE_LOG(LogCrowdyStudio, Display,
				TEXT("Effect plan cache: reused function '%s' differs from a fresh compile of '%s'."),
				*CachedFunctions[Index].Name, *FreshFunctions[Index].Name);
			bIdentical = false;
		}
	}
	TestTrue(TEXT("A plan built from stored results matches one compiled with no cache, field for field"), bIdentical);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerLoadAlwaysCompletesTest,
	"CrowdySDK.CrowdyStudio.ContainerLoadAlwaysCompletes", CrowdySchemaGatherBaselineTestFlags)

bool FCrowdyContainerLoadAlwaysCompletesTest::RunTest(const FString& Parameters)
{
	// A schema plan now CONTINUES from this request's completion instead of running straight on, so a path that
	// silently declines to call it does not merely skip a load: it strands the plan forever, with the page left
	// saying a check is under way and no way to start another. The no-hook path is the one that used to just log and
	// return, and it is reachable whenever CrowdySDKEditor is not loaded.
	//
	// The hook is process-wide, so it is restored before returning by every path out of this test.
	struct FHookGuard
	{
		explicit FHookGuard(TFunction<void(TFunction<void()>)> InSaved) : Saved(MoveTemp(InSaved)) {}
		~FHookGuard() { CrowdyStudioRegistry::SetLoadContainerAssetsHook(MoveTemp(Saved)); }
		TFunction<void(TFunction<void()>)> Saved;
	};

	bool bCompletedWithNoHook = false;
	bool bCompletedThroughHook = false;
	{
		FHookGuard Guard(CrowdyStudioRegistry::GetLoadContainerAssetsHook());

		// The no-hook path: reachable whenever CrowdySDKEditor is not loaded, and the one that used to log and return.
		CrowdyStudioRegistry::SetLoadContainerAssetsHook(nullptr);
		CrowdyStudioRegistry::RequestLoadContainerAssets([&bCompletedWithNoHook]() { bCompletedWithNoHook = true; });

		// And an installed hook must have the completion handed through to it rather than dropped on the way.
		CrowdyStudioRegistry::SetLoadContainerAssetsHook(
			[](TFunction<void()> OnComplete) { if (OnComplete) OnComplete(); });
		CrowdyStudioRegistry::RequestLoadContainerAssets([&bCompletedThroughHook]() { bCompletedThroughHook = true; });
	}

	TestTrue(TEXT("A request with no hook still completes, so a plan behind it is never stranded"),
		bCompletedWithNoHook);
	TestTrue(TEXT("A request with a hook completes through it"), bCompletedThroughHook);

	return true;
}

#endif
