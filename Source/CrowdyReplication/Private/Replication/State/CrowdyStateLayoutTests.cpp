#include "Replication/State/CrowdyStateTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Replication/RPC/CrowdyRPC.h"
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyStateTestFlags =
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
}

// The builder collects the marked properties in reflection (declaration) order, matches the annotated
// set (the unmarked property stays out), and parses the per-property owner-only / manual-dirty / onrep
// flags from the metadata.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLayoutDeclarationOrderTest,
	"CrowdySDK.State.LayoutDeclarationOrder", CrowdyStateTestFlags)
bool FCrowdyStateLayoutDeclarationOrderTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyStateTestTarget::StaticClass();

	// The positional wire order is the source declaration order. This is the hand-known order the
	// properties are declared in CrowdyStateTestTarget.h, grounded independently of the builder's own
	// TFieldIterator walk, so a builder that reordered (or an engine whose iteration diverged from
	// declaration order) would be caught rather than passing in lockstep with a re-derived expectation.
	const TArray<FName> ExpectedOrder = {
		TEXT("RepInt"), TEXT("RepBigInt"), TEXT("RepFloat"), TEXT("RepDouble"),
		TEXT("bRepFlag"), TEXT("RepByte"), TEXT("RepEnum"), TEXT("RepName"),
		TEXT("RepString"), TEXT("RepVector"), TEXT("RepRotator"),
		TEXT("RepOwnerOnly"), TEXT("RepManualDirty"), TEXT("RepHealth")
	};

	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout built"), FCrowdyStateLayoutBuilder::BuildLayout(Class, Layout));
	TestTrue(TEXT("layout valid"), Layout.IsValid());
	TestTrue(TEXT("owner class recorded"), Layout.OwnerClass.Get() == Class);
	TestEqual(TEXT("count matches marked set"), Layout.Properties.Num(), ExpectedOrder.Num());

	const int32 Count = FMath::Min(Layout.Properties.Num(), ExpectedOrder.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		if (TestNotNull(*FString::Printf(TEXT("property %d resolved"), i), Layout.Properties[i].Property))
		{
			TestTrue(*FString::Printf(TEXT("order matches at %d"), i),
				Layout.Properties[i].Property->GetFName() == ExpectedOrder[i]);
		}
	}

	// A spot-check of the annotated set: known members in, the unmarked one out.
	TestNotNull(TEXT("RepInt present"), FindByName(Layout, TEXT("RepInt")));
	TestNotNull(TEXT("RepEnum present"), FindByName(Layout, TEXT("RepEnum")));
	TestNotNull(TEXT("RepString present"), FindByName(Layout, TEXT("RepString")));
	TestNotNull(TEXT("RepVector present"), FindByName(Layout, TEXT("RepVector")));
	TestNull(TEXT("unmarked property excluded"), FindByName(Layout, TEXT("NotReplicated")));

	if (const FCrowdyRepProperty* OwnerOnly = FindByName(Layout, TEXT("RepOwnerOnly")))
	{
		TestTrue(TEXT("owner-only flag set"), OwnerOnly->bOwnerOnly);
		TestFalse(TEXT("owner-only not manual-dirty"), OwnerOnly->bManualDirty);
		TestTrue(TEXT("owner-only has no onrep"), OwnerOnly->OnRepFunctionName.IsNone());
	}
	else
	{
		AddError(TEXT("RepOwnerOnly missing from layout"));
	}

	if (const FCrowdyRepProperty* ManualDirty = FindByName(Layout, TEXT("RepManualDirty")))
	{
		TestTrue(TEXT("manual-dirty flag set"), ManualDirty->bManualDirty);
		TestFalse(TEXT("manual-dirty not owner-only"), ManualDirty->bOwnerOnly);
	}
	else
	{
		AddError(TEXT("RepManualDirty missing from layout"));
	}

	if (const FCrowdyRepProperty* Health = FindByName(Layout, TEXT("RepHealth")))
	{
		TestTrue(TEXT("onrep name parsed"), Health->OnRepFunctionName == FName(TEXT("OnRep_Health")));
		TestTrue(TEXT("heartbeat flag set"), Health->bHeartbeat);
	}
	else
	{
		AddError(TEXT("RepHealth missing from layout"));
	}

	if (const FCrowdyRepProperty* Plain = FindByName(Layout, TEXT("RepInt")))
	{
		TestFalse(TEXT("plain property not owner-only"), Plain->bOwnerOnly);
		TestFalse(TEXT("plain property not manual-dirty"), Plain->bManualDirty);
		TestFalse(TEXT("plain property not heartbeat"), Plain->bHeartbeat);
		TestTrue(TEXT("plain property has no onrep"), Plain->OnRepFunctionName.IsNone());
	}

	return true;
}

// Building the same layout twice yields identical, non-zero, distinct PropertyIDs and an identical,
// non-zero LayoutHash; IndexOfPropertyID round-trips each id to its position.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStatePropertyIDStableTest,
	"CrowdySDK.State.PropertyIDStable", CrowdyStateTestFlags)
bool FCrowdyStatePropertyIDStableTest::RunTest(const FString& Parameters)
{
	UClass* Class = UCrowdyStateTestTarget::StaticClass();

	FCrowdyRepLayout A;
	FCrowdyRepLayout B;
	TestTrue(TEXT("build A"), FCrowdyStateLayoutBuilder::BuildLayout(Class, A));
	TestTrue(TEXT("build B"), FCrowdyStateLayoutBuilder::BuildLayout(Class, B));

	TestEqual(TEXT("same property count"), A.Properties.Num(), B.Properties.Num());
	TestNotEqual(TEXT("layout hash is non-zero"), A.LayoutHash, static_cast<int64>(0));
	TestEqual(TEXT("layout hash is stable"), A.LayoutHash, B.LayoutHash);

	TSet<int64> Seen;
	const int32 Count = FMath::Min(A.Properties.Num(), B.Properties.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		const int64 ID = A.Properties[i].PropertyID;
		TestNotEqual(*FString::Printf(TEXT("id non-zero at %d"), i), ID, static_cast<int64>(0));
		TestEqual(*FString::Printf(TEXT("id stable at %d"), i), ID, B.Properties[i].PropertyID);
		TestFalse(*FString::Printf(TEXT("id distinct at %d"), i), Seen.Contains(ID));
		Seen.Add(ID);
		TestEqual(*FString::Printf(TEXT("index-of round-trips at %d"), i), A.IndexOfPropertyID(ID), i);
	}

	// A PropertyID is never zero (the generator maps a zero hash to 1), so zero is always a miss.
	TestEqual(TEXT("unknown id resolves to INDEX_NONE"), A.IndexOfPropertyID(0), static_cast<int32>(INDEX_NONE));
	return true;
}

// Two real classes differing by one added replicated property produce different LayoutHash values.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateLayoutHashChangesOnAddTest,
	"CrowdySDK.State.LayoutHashChangesOnAdd", CrowdyStateTestFlags)
bool FCrowdyStateLayoutHashChangesOnAddTest::RunTest(const FString& Parameters)
{
	FCrowdyRepLayout Base;
	FCrowdyRepLayout Extra;
	TestTrue(TEXT("build base"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateTestTarget::StaticClass(), Base));
	TestTrue(TEXT("build extra"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateTestTargetExtra::StaticClass(), Extra));

	// The subclass layout is the parent's set plus exactly one property (super-class inclusion).
	TestEqual(TEXT("extra has one more property"), Extra.Properties.Num(), Base.Properties.Num() + 1);

	// Adding a replicated property changes the positional guard.
	TestNotEqual(TEXT("layout hash changed on add"), Base.LayoutHash, Extra.LayoutHash);
	return true;
}

// The canonical *type* token participates in both PropertyID and LayoutHash, so a retype (same slot,
// same name, different type) changes identity and the positional guard, a drifted peer drops instead
// of misparsing. The two fixtures differ ONLY by their single property's type; LayoutHash excludes the
// class path, so a hash difference isolates the type token, and each PropertyID is pinned to the full
// documented formula so a dropped type fragment there is caught too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateTypeParticipatesInIdentityTest,
	"CrowdySDK.State.TypeParticipatesInIdentity", CrowdyStateTestFlags)
bool FCrowdyStateTypeParticipatesInIdentityTest::RunTest(const FString& Parameters)
{
	UClass* IntClass = UCrowdyStateRetypeIntTarget::StaticClass();
	UClass* Int64Class = UCrowdyStateRetypeInt64Target::StaticClass();

	FCrowdyRepLayout IntLayout;
	FCrowdyRepLayout Int64Layout;
	TestTrue(TEXT("build int32 fixture"), FCrowdyStateLayoutBuilder::BuildLayout(IntClass, IntLayout));
	TestTrue(TEXT("build int64 fixture"), FCrowdyStateLayoutBuilder::BuildLayout(Int64Class, Int64Layout));

	if (IntLayout.Properties.Num() != 1 || Int64Layout.Properties.Num() != 1)
	{
		AddError(TEXT("retype fixtures must each expose exactly one replicated property"));
		return false;
	}

	// LayoutHash folds "name:type" and excludes the class path, so two fixtures identical but for the
	// property's type must hash differently. A builder that dropped the type fragment from the hash
	// source would hash "RepValue" for both, and this would fail , isolating the type token.
	TestNotEqual(TEXT("retype changes LayoutHash"), IntLayout.LayoutHash, Int64Layout.LayoutHash);

	// Pin each PropertyID to the full documented formula (class path + name + canonical type) so a
	// builder that dropped the type token from the id is caught.
	const auto GoldenID = [](UClass* OwnerClass, const FCrowdyRepProperty& Prop) -> int64
	{
		const FString Source = OwnerClass->GetPathName() + TEXT("::") + Prop.Property->GetName()
			+ TEXT(":") + FCrowdyRPC::CanonicalParamType(Prop.Property);
		return FCrowdyTypeIDGenerator::GenerateFromString(Source);
	};
	TestEqual(TEXT("int32 PropertyID includes type token"),
		IntLayout.Properties[0].PropertyID, GoldenID(IntClass, IntLayout.Properties[0]));
	TestEqual(TEXT("int64 PropertyID includes type token"),
		Int64Layout.Properties[0].PropertyID, GoldenID(Int64Class, Int64Layout.Properties[0]));

	// The two ids differ (here via both class path and type), a basic distinctness sanity check.
	TestNotEqual(TEXT("retype yields distinct PropertyIDs"),
		IntLayout.Properties[0].PropertyID, Int64Layout.Properties[0].PropertyID);
	return true;
}

// An object-reference property marked CrowdyState is rejected with an error and omitted; the other
// (accepted) property survives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateRejectsObjectRefTest,
	"CrowdySDK.State.RejectsObjectRef", CrowdyStateTestFlags)
bool FCrowdyStateRejectsObjectRefTest::RunTest(const FString& Parameters)
{
	// The reject is a deliberate error log; whitelist it so it does not fail the test, and require
	// (Occurrences == 0 means "one or more") that it actually fired.
	AddExpectedError(TEXT("object/interface/delegate reference"), EAutomationExpectedErrorFlags::Contains, 0);

	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout still valid after reject"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateObjectRejectTarget::StaticClass(), Layout));

	TestNotNull(TEXT("accepted property survives"), FindByName(Layout, TEXT("RepGood")));
	TestNull(TEXT("object-ref property omitted"), FindByName(Layout, TEXT("RepBadObject")));
	TestEqual(TEXT("only the accepted property remains"), Layout.Properties.Num(), 1);
	return true;
}

// A container property marked CrowdyState is rejected with an error and omitted; the other (accepted)
// property survives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateRejectsContainerTest,
	"CrowdySDK.State.RejectsContainer", CrowdyStateTestFlags)
bool FCrowdyStateRejectsContainerTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("CrowdyState does not replicate containers"), EAutomationExpectedErrorFlags::Contains, 0);

	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout still valid after reject"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateContainerRejectTarget::StaticClass(), Layout));

	TestNotNull(TEXT("accepted property survives"), FindByName(Layout, TEXT("RepGood")));
	TestNull(TEXT("container property omitted"), FindByName(Layout, TEXT("RepBadArray")));
	TestEqual(TEXT("only the accepted property remains"), Layout.Properties.Num(), 1);
	return true;
}

// A meta=(CrowdyState) USTRUCT that transitively contains a container is rejected with an error and
// omitted (remote-OOM hardening: the inner container's untrusted count would drive an unbounded decode
// allocation on the SerializeItem path); the accepted scalar survives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateRejectsNestedContainerTest,
	"CrowdySDK.State.RejectsNestedContainer", CrowdyStateTestFlags)
bool FCrowdyStateRejectsNestedContainerTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("transitively contains a container"), EAutomationExpectedErrorFlags::Contains, 0);

	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout still valid after reject"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateNestedContainerRejectTarget::StaticClass(), Layout));

	TestNotNull(TEXT("accepted property survives"), FindByName(Layout, TEXT("RepGood")));
	TestNull(TEXT("nested-container struct omitted"), FindByName(Layout, TEXT("RepBadStruct")));
	TestEqual(TEXT("only the accepted property remains"), Layout.Properties.Num(), 1);
	return true;
}

// A fixed-size C array marked CrowdyState is rejected with an error and omitted (a positional two-argument
// Identical would compare only element [0], silently dropping changes to later elements); the accepted
// scalar survives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateRejectsStaticArrayTest,
	"CrowdySDK.State.RejectsStaticArray", CrowdyStateTestFlags)
bool FCrowdyStateRejectsStaticArrayTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("is a fixed-size array"), EAutomationExpectedErrorFlags::Contains, 0);

	FCrowdyRepLayout Layout;
	TestTrue(TEXT("layout still valid after reject"),
		FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyStateStaticArrayRejectTarget::StaticClass(), Layout));

	TestNotNull(TEXT("accepted property survives"), FindByName(Layout, TEXT("RepGood")));
	TestNull(TEXT("static-array property omitted"), FindByName(Layout, TEXT("RepBadArray")));
	TestEqual(TEXT("only the accepted property remains"), Layout.Properties.Num(), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
