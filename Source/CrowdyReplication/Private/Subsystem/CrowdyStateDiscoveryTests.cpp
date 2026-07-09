#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/GameInstance.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Subsystem/CrowdyAutoRegistry.h"
#include "Utils/CrowdyBakedRegistry.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStateDiscoveryTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const FCrowdyRepProperty* FindByName(const FCrowdyRepLayout& Layout, const TCHAR* Name)
	{
		const FName Wanted(Name);
		for (const FCrowdyRepProperty& Prop : Layout.Properties)
		{
			if (Prop.Property && Prop.Property->GetFName() == Wanted)
			{
				return &Prop;
			}
		}
		return nullptr;
	}

	// UCrowdyAutoRegistry is a UGameInstanceSubsystem (ClassWithin=UGameInstance), so it must be outered
	// to a UGameInstance NewObject into the transient package trips a ClassWithin ensure. A bare,
	// uninitialized GameInstance suffices here: the rep-layout methods use only reflection and the global
	// class/baked registries, never GetGameInstance()/GetWorld().
	UCrowdyAutoRegistry* MakeStateRegistry()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
		return NewObject<UCrowdyAutoRegistry>(GameInstance);
	}
}

// Discovery deliverable: a fresh registry scan finds the fixture's meta=(CrowdyState) properties and
// caches a layout FindRepLayout returns. The layout is exactly the annotated set (the unmarked
// property stays out), and the per-property owner-only / manual-dirty / onrep flags are parsed. In an
// editor build FindRepLayout assembles the layout live, so this also proves the cache serves the
// annotated layout back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateDiscoveryFindsAnnotatedTest,
	"CrowdySDK.State.DiscoveryFindsAnnotated", CrowdyStateDiscoveryTestFlags)
bool FCrowdyStateDiscoveryFindsAnnotatedTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyStateTestTarget::StaticClass();

	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	TestNotNull(TEXT("registry created"), Registry);
	if (!Registry)
	{
		return false;
	}

	// RescanRepLayouts() sweeps every loaded UClass, which includes the sibling reject fixtures
	// (UCrowdyStateObjectRejectTarget's object-ref, UCrowdyStateContainerRejectTarget's container). The
	// builder logs an error for each rejected property, and an error logged during a running automation
	// test fails it unless whitelisted so declare both as expected (Occurrences 0 means one or more).
	AddExpectedError(TEXT("object/interface/delegate reference"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("CrowdyState does not replicate containers"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("transitively contains a container"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("is a fixed-size array"), EAutomationExpectedErrorFlags::Contains, 0);
	// The sweep also covers ACrowdyStateOverlapActor, whose executor-overlap filter drops Health with an
	// error; whitelist it here too.
	AddExpectedError(TEXT("also lives in its executor state struct"), EAutomationExpectedErrorFlags::Contains, 0);
	// The sweep also covers UCrowdyStateBadOnRepTarget, whose parameter-taking OnRep is cleared with an
	// error by ValidateOnRepSignatures; whitelist it (one line covers both the missing and non-parameterless cases).
	AddExpectedError(TEXT("is not a valid CrowdyOnRep notify"), EAutomationExpectedErrorFlags::Contains, 0);

	Registry->RescanRepLayouts();

	const FCrowdyRepLayout* Layout = Registry->FindRepLayout(Class);
	TestNotNull(TEXT("annotated class has a discovered layout"), Layout);
	if (!Layout)
	{
		return false;
	}

	TestTrue(TEXT("layout is valid"), Layout->IsValid());
	TestTrue(TEXT("owner class recorded"), Layout->OwnerClass.Get() == Class);

	// The fourteen meta=(CrowdyState) members of the fixture, and nothing else.
	TestEqual(TEXT("count matches the annotated set"), Layout->Properties.Num(), 14);

	// Every annotated member is present; the unmarked one is not.
	for (const TCHAR* Name : { TEXT("RepInt"), TEXT("RepBigInt"), TEXT("RepFloat"), TEXT("RepDouble"),
		TEXT("bRepFlag"), TEXT("RepByte"), TEXT("RepEnum"), TEXT("RepName"), TEXT("RepString"),
		TEXT("RepVector"), TEXT("RepRotator"), TEXT("RepOwnerOnly"), TEXT("RepManualDirty"),
		TEXT("RepHealth") })
	{
		TestNotNull(*FString::Printf(TEXT("%s present"), Name), FindByName(*Layout, Name));
	}
	TestNull(TEXT("unmarked property excluded"), FindByName(*Layout, TEXT("NotReplicated")));

	// The per-property flags and onrep name are parsed from the metadata.
	if (const FCrowdyRepProperty* OwnerOnly = FindByName(*Layout, TEXT("RepOwnerOnly")))
	{
		TestTrue(TEXT("owner-only flag set"), OwnerOnly->bOwnerOnly);
		TestFalse(TEXT("owner-only not manual-dirty"), OwnerOnly->bManualDirty);
		TestTrue(TEXT("owner-only has no onrep"), OwnerOnly->OnRepFunctionName.IsNone());
	}

	if (const FCrowdyRepProperty* ManualDirty = FindByName(*Layout, TEXT("RepManualDirty")))
	{
		TestTrue(TEXT("manual-dirty flag set"), ManualDirty->bManualDirty);
		TestFalse(TEXT("manual-dirty not owner-only"), ManualDirty->bOwnerOnly);
	}

	if (const FCrowdyRepProperty* Health = FindByName(*Layout, TEXT("RepHealth")))
	{
		TestTrue(TEXT("onrep name parsed"), Health->OnRepFunctionName == FName(TEXT("OnRep_Health")));
		TestTrue(TEXT("heartbeat flag set"), Health->bHeartbeat);
	}

	if (const FCrowdyRepProperty* Plain = FindByName(*Layout, TEXT("RepInt")))
	{
		TestFalse(TEXT("plain property not owner-only"), Plain->bOwnerOnly);
		TestFalse(TEXT("plain property not manual-dirty"), Plain->bManualDirty);
		TestFalse(TEXT("plain property not heartbeat"), Plain->bHeartbeat);
		TestTrue(TEXT("plain property has no onrep"), Plain->OnRepFunctionName.IsNone());
	}

	// A second lookup returns the cached layout, FindRepLayout does not rebuild on every call.
	const FCrowdyRepLayout* Again = Registry->FindRepLayout(Class);
	TestEqual(TEXT("second lookup returns the cached layout"), Again, Layout);

	return true;
}

// Bake round-trip: a live layout, baked to FCrowdyBakedRepProperty rows via the shared factory and read
// back through BuildLayoutFromBaked, reproduces the same layout the runtime computes live same count,
// same order, same per-property id/flags/onrep, each FProperty* re-resolved by name, and the same
// LayoutHash. This is the editor<->cooked parity the cooked FindRepLayout path relies on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateBakeRoundTripTest,
	"CrowdySDK.State.BakeRoundTrip", CrowdyStateDiscoveryTestFlags)
bool FCrowdyStateBakeRoundTripTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyStateTestTarget::StaticClass();
	const FSoftClassPath Path(Class);

	FCrowdyRepLayout Live;
	TestTrue(TEXT("live layout built"), FCrowdyStateLayoutBuilder::BuildLayout(Class, Live));
	TestTrue(TEXT("live layout valid"), Live.IsValid());

	UCrowdyBakedRegistry* Baked = NewObject<UCrowdyBakedRegistry>(GetTransientPackage());
	TestNotNull(TEXT("baked registry created"), Baked);
	if (!Baked)
	{
		return false;
	}

	// Bake exactly the way the cook would, through the shared live->baked factory.
	UCrowdyBakedRegistry::MakeBakedRepProperties(Live, Path, Baked->RepProperties);
	Baked->RepLayoutHashes.Add({ Path, Live.LayoutHash });

	TestEqual(TEXT("one baked row per live property"), Baked->RepProperties.Num(), Live.Properties.Num());

	FCrowdyRepLayout Assembled;
	TestTrue(TEXT("layout assembled from baked table"),
		UCrowdyAutoRegistry::BuildLayoutFromBaked(Class, Baked, Assembled));
	TestTrue(TEXT("assembled layout valid"), Assembled.IsValid());
	TestTrue(TEXT("assembled owner class recorded"), Assembled.OwnerClass.Get() == Class);

	TestEqual(TEXT("assembled property count matches live"),
		Assembled.Properties.Num(), Live.Properties.Num());
	TestEqual(TEXT("assembled layout hash matches live"), Assembled.LayoutHash, Live.LayoutHash);

	const int32 Count = FMath::Min(Assembled.Properties.Num(), Live.Properties.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		const FCrowdyRepProperty& L = Live.Properties[i];
		const FCrowdyRepProperty& A = Assembled.Properties[i];

		// The FProperty* is re-resolved by name against the class, never serialized.
		if (!TestNotNull(*FString::Printf(TEXT("assembled property %d re-resolved"), i), A.Property))
		{
			continue;
		}
		TestNotNull(*FString::Printf(TEXT("live property %d present"), i), L.Property);

		if (L.Property && A.Property)
		{
			// Same positional order: identical property at each slot.
			TestTrue(*FString::Printf(TEXT("order matches at %d"), i),
				A.Property->GetFName() == L.Property->GetFName());
		}

		TestEqual(*FString::Printf(TEXT("property id survives the bake at %d"), i),
			A.PropertyID, L.PropertyID);
		TestEqual(*FString::Printf(TEXT("owner-only flag survives at %d"), i),
			A.bOwnerOnly, L.bOwnerOnly);
		TestEqual(*FString::Printf(TEXT("manual-dirty flag survives at %d"), i),
			A.bManualDirty, L.bManualDirty);
			TestEqual(*FString::Printf(TEXT("heartbeat flag survives at %d"), i),
				A.bHeartbeat, L.bHeartbeat);
		TestTrue(*FString::Printf(TEXT("onrep name survives at %d"), i),
			A.OnRepFunctionName == L.OnRepFunctionName);
	}

	return true;
}

// Incremental update: refreshing one class's layout twice (evict + rebuild) reproduces an identical
// layout each time the same count, same per-index PropertyID, same LayoutHash matching a fresh builder
// layout. This proves the incremental path does not depend on a full sweep, mirroring the RPC
// UpdateClassRpcFunctions contract. Values, not pointers, are compared: the rebuild yields a new
// allocation, and only the reproduced content must match.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateIncrementalUpdateTest,
	"CrowdySDK.State.IncrementalUpdate", CrowdyStateDiscoveryTestFlags)
bool FCrowdyStateIncrementalUpdateTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyStateTestTarget::StaticClass();

	// The independent ground truth: a layout built directly from reflection.
	FCrowdyRepLayout Fresh;
	TestTrue(TEXT("fresh layout built"), FCrowdyStateLayoutBuilder::BuildLayout(Class, Fresh));
	TestTrue(TEXT("fresh layout valid"), Fresh.IsValid());

	UCrowdyAutoRegistry* Registry = MakeStateRegistry();
	TestNotNull(TEXT("registry created"), Registry);
	if (!Registry)
	{
		return false;
	}

	// First incremental refresh of just this class (no full sweep).
	Registry->UpdateClassRepLayout(Class);
	const FCrowdyRepLayout* First = Registry->FindRepLayout(Class);
	TestNotNull(TEXT("first refresh produced a layout"), First);

	// Second incremental refresh evicts and rebuilds the same class.
	Registry->UpdateClassRepLayout(Class);
	const FCrowdyRepLayout* Second = Registry->FindRepLayout(Class);
	TestNotNull(TEXT("second refresh produced a layout"), Second);
	if (!Second)
	{
		return false;
	}

	// The rebuilt layout reproduces the fresh builder layout by value.
	TestEqual(TEXT("rebuilt count matches fresh"), Second->Properties.Num(), Fresh.Properties.Num());
	TestNotEqual(TEXT("layout hash is non-zero"), Second->LayoutHash, static_cast<int64>(0));
	TestEqual(TEXT("rebuilt layout hash matches fresh"), Second->LayoutHash, Fresh.LayoutHash);

	const int32 Count = FMath::Min(Second->Properties.Num(), Fresh.Properties.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		TestEqual(*FString::Printf(TEXT("rebuilt property id matches fresh at %d"), i),
			Second->Properties[i].PropertyID, Fresh.Properties[i].PropertyID);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
