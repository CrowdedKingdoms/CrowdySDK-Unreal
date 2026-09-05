// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/GameModel/CrowdyModelAttributeLookup.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"
#include "Replication/State/FCrowdyRepLayout.h"
#include "Utils/CrowdyBakedRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyAttributeTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	const FCrowdyAttributeDef* FindDef(const TArray<FCrowdyAttributeDef>& Defs, const TCHAR* Key)
	{
		return Defs.FindByPredicate([Key](const FCrowdyAttributeDef& D) { return D.Key == Key; });
	}
}

// Discovery reflects exactly the accepted CrowdyModel attributes: the four value-type mappings, native clamp,
// a resolved notify, a dropped wrong-arity notify (attribute kept), and it REJECTS a dual-plane property and
// an unsupported type. The two rejects log errors -- whitelisted so the discovery scan does not fail the test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelAttributeDiscoveryTest,
	"CrowdySDK.GameModel.AttributeDiscovery", CrowdyAttributeTestFlags)
bool FCrowdyGameModelAttributeDiscoveryTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("marked both CrowdyModel and CrowdyState"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("must be parameterless"), EAutomationExpectedErrorFlags::Contains, 1);

	const TArray<FCrowdyAttributeDef> Defs =
		FCrowdyAttributeRegistry::DiscoverForClass(UCrowdyGameModelDiscoveryTarget::StaticClass());

	// Exactly the six accepted attributes (health, speed, bstunned, title, tier, badnotify).
	TestEqual(TEXT("accepted attribute count"), Defs.Num(), 6);

	// Excluded: dual-plane (exclusivity), unsupported type (TMap), plain property.
	TestNull(TEXT("dual-plane property rejected"), FindDef(Defs, TEXT("dualplane")));
	TestNull(TEXT("unsupported TMap rejected"), FindDef(Defs, TEXT("bag")));
	TestNull(TEXT("plain property never a CrowdyModel"), FindDef(Defs, TEXT("plainint")));

	// health: int, native clamp [0,100], parameterless notify resolved.
	if (const FCrowdyAttributeDef* Health = FindDef(Defs, TEXT("health")))
	{
		TestEqual(TEXT("health valueType"), Health->ValueType, FString(TEXT("int")));
		TestTrue(TEXT("health has clamp"), Health->bHasClamp);
		TestEqual(TEXT("health clamp min"), Health->ClampMin, 0.0);
		TestEqual(TEXT("health clamp max"), Health->ClampMax, 100.0);
		TestEqual(TEXT("health onrep resolved"), Health->OnRepFunctionName, FName(TEXT("OnRep_Health")));
	}
	else
	{
		AddError(TEXT("health attribute missing"));
	}

	// Value-type spread.
	if (const FCrowdyAttributeDef* Speed = FindDef(Defs, TEXT("speed")))
	{
		TestEqual(TEXT("speed valueType"), Speed->ValueType, FString(TEXT("float")));
		TestFalse(TEXT("speed has no clamp"), Speed->bHasClamp);
	}
	if (const FCrowdyAttributeDef* Stunned = FindDef(Defs, TEXT("bstunned")))
	{
		TestEqual(TEXT("bstunned valueType"), Stunned->ValueType, FString(TEXT("bool")));
	}
	if (const FCrowdyAttributeDef* Title = FindDef(Defs, TEXT("title")))
	{
		TestEqual(TEXT("title valueType"), Title->ValueType, FString(TEXT("string")));
	}
	// An enum class : uint8 maps to "int" (parity with a byte attribute), not dropped.
	if (const FCrowdyAttributeDef* Tier = FindDef(Defs, TEXT("tier")))
	{
		TestEqual(TEXT("enum tier valueType"), Tier->ValueType, FString(TEXT("int")));
	}
	else
	{
		AddError(TEXT("enum attribute 'tier' missing (an enum class : uint8 must map to int, not be dropped)"));
	}

	// badnotify: accepted attribute, but the one-arg notify was dropped (kept replicating, no notify).
	if (const FCrowdyAttributeDef* Bad = FindDef(Defs, TEXT("badnotify")))
	{
		TestEqual(TEXT("badnotify valueType"), Bad->ValueType, FString(TEXT("int")));
		TestEqual(TEXT("badnotify wrong-arity notify dropped"), Bad->OnRepFunctionName, FName(NAME_None));
	}
	else
	{
		AddError(TEXT("badnotify attribute missing (the wrong-arity notify should drop only the notify, not the attribute)"));
	}

	return true;
}

// A CrowdyKey override replaces the derived server key, and CrowdyVisibility sets the server read-visibility
// (public|owner|hidden), with an unrecognized value falling back to public (a warning, not an error). These
// feed the schema sync's property def; the runtime cache does not read visibility.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelAttributeKeyVisibilityTest,
	"CrowdySDK.GameModel.AttributeKeyAndVisibility", CrowdyAttributeTestFlags)
bool FCrowdyGameModelAttributeKeyVisibilityTest::RunTest(const FString& Parameters)
{
	const TArray<FCrowdyAttributeDef> Defs =
		FCrowdyAttributeRegistry::DiscoverForClass(UCrowdyGameModelKeyVisTarget::StaticClass());

	TestEqual(TEXT("five accepted attributes"), Defs.Num(), 5);

	// CrowdyKey override: the "Reserve" property is keyed "ammo", and the derived "reserve" never appears.
	TestNotNull(TEXT("overridden key 'ammo' present"), FindDef(Defs, TEXT("ammo")));
	TestNull(TEXT("derived 'reserve' key replaced by the override"), FindDef(Defs, TEXT("reserve")));

	// Visibility variants.
	if (const FCrowdyAttributeDef* Secret = FindDef(Defs, TEXT("secretscore")))
	{
		TestEqual(TEXT("owner visibility"), Secret->Visibility, FString(TEXT("owner")));
	}
	else
	{
		AddError(TEXT("secretscore attribute missing"));
	}
	if (const FCrowdyAttributeDef* Admin = FindDef(Defs, TEXT("adminnote")))
	{
		TestEqual(TEXT("hidden visibility"), Admin->Visibility, FString(TEXT("hidden")));
	}
	// An unrecognized visibility falls back to public (a warning is logged, not an error).
	if (const FCrowdyAttributeDef* Mistyped = FindDef(Defs, TEXT("mistyped")))
	{
		TestEqual(TEXT("invalid visibility falls back to public"), Mistyped->Visibility, FString(TEXT("public")));
	}
	// No CrowdyVisibility -> the public default.
	if (const FCrowdyAttributeDef* Public = FindDef(Defs, TEXT("publicscore")))
	{
		TestEqual(TEXT("default visibility is public"), Public->Visibility, FString(TEXT("public")));
	}

	return true;
}

// The duplicate-server-key guard: two CrowdyModel attributes resolving to the same server key keep only the
// first seen; the second is dropped with an error. Reachable because a CrowdyKey override can force the
// collision. Robust to field iteration order: exactly one "ammo" attribute survives, never both.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelAttributeDuplicateKeyTest,
	"CrowdySDK.GameModel.AttributeDuplicateKey", CrowdyAttributeTestFlags)
bool FCrowdyGameModelAttributeDuplicateKeyTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("already used by"), EAutomationExpectedErrorFlags::Contains, 1);

	const TArray<FCrowdyAttributeDef> Defs =
		FCrowdyAttributeRegistry::DiscoverForClass(UCrowdyGameModelDupKeyTarget::StaticClass());

	TestEqual(TEXT("one attribute survives the duplicate-key collision"), Defs.Num(), 1);
	TestNotNull(TEXT("the surviving attribute is keyed 'ammo'"), FindDef(Defs, TEXT("ammo")));

	return true;
}

// The container-type class tag resolves for a CrowdyContainer class and is absent otherwise.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelContainerTagTest,
	"CrowdySDK.GameModel.ContainerTag", CrowdyAttributeTestFlags)
bool FCrowdyGameModelContainerTagTest::RunTest(const FString& Parameters)
{
	FString TypeName;
	TestTrue(TEXT("discovery target has a container tag"),
		FCrowdyAttributeRegistry::GetContainerTypeName(UCrowdyGameModelDiscoveryTarget::StaticClass(), TypeName));
	TestEqual(TEXT("container type name"), TypeName, FString(TEXT("TestHero")));

	FString None;
	TestFalse(TEXT("a plain UObject has no container tag"),
		FCrowdyAttributeRegistry::GetContainerTypeName(UObject::StaticClass(), None));

	TestTrue(TEXT("discovery target reports model attributes"),
		FCrowdyAttributeRegistry::ClassHasModelAttributes(UCrowdyGameModelDiscoveryTarget::StaticClass()));
	TestFalse(TEXT("a plain UObject reports no model attributes"),
		FCrowdyAttributeRegistry::ClassHasModelAttributes(UObject::StaticClass()));

	return true;
}

// Plane-exclusivity is SYMMETRIC: a property marked BOTH CrowdyModel and CrowdyState lands in NEITHER plane.
// The Game Model discovery drops it (asserted in AttributeDiscovery); this proves the CrowdyState layout
// builder ALSO drops it, so a mismarked cheat-sensitive field can never silently ship on the fast
// client-authoritative view plane (the inverse of the two-plane invariant).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelPlaneExclusivitySymmetricTest,
	"CrowdySDK.GameModel.PlaneExclusivitySymmetric", CrowdyAttributeTestFlags)
bool FCrowdyGameModelPlaneExclusivitySymmetricTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("marked both CrowdyState and CrowdyModel"), EAutomationExpectedErrorFlags::Contains, 0);

	FCrowdyRepLayout Layout;
	FCrowdyStateLayoutBuilder::BuildLayout(UCrowdyGameModelDiscoveryTarget::StaticClass(), Layout);

	// The discovery target's only CrowdyState-marked property is DualPlane (also CrowdyModel) -> excluded, so
	// no CrowdyState property survives on this class.
	for (const FCrowdyRepProperty& P : Layout.Properties)
	{
		if (P.Property)
		{
			TestNotEqual(TEXT("dual-plane property excluded from the CrowdyState layout"),
				P.Property->GetName(), FString(TEXT("DualPlane")));
		}
	}
	TestEqual(TEXT("no CrowdyState property survives on the dual-marked class"), Layout.Properties.Num(), 0);

	return true;
}

// The attribute bake round-trips: baking the discovery target's live defs through the shared
// MakeBakedAttributes factory and reading them back through the asset-local FindModelAttributes /
// FindContainerTypeName reproduces exactly what live DiscoverForClass / GetContainerTypeName produce.
// This is the editor<->cooked parity the cooked (metadata-stripped) attribute path relies on -- the
// #else branches of DiscoverForClass / GetContainerTypeName / ClassHasModelAttributes read this table.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelAttributeBakeRoundTripTest,
	"CrowdySDK.GameModel.AttributeBakeRoundTrip", CrowdyAttributeTestFlags)
bool FCrowdyGameModelAttributeBakeRoundTripTest::RunTest(const FString& Parameters)
{
	// DiscoverForClass on the fixture logs the dual-plane reject + the wrong-arity notify drop.
	AddExpectedError(TEXT("marked both CrowdyModel and CrowdyState"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("must be parameterless"), EAutomationExpectedErrorFlags::Contains, 1);

	UClass* Class = UCrowdyGameModelDiscoveryTarget::StaticClass();
	const FSoftClassPath Path(Class);

	const TArray<FCrowdyAttributeDef> Live = FCrowdyAttributeRegistry::DiscoverForClass(Class);
	TestTrue(TEXT("live discovery non-empty"), Live.Num() > 0);

	FString LiveType;
	TestTrue(TEXT("live container tag resolves"), FCrowdyAttributeRegistry::GetContainerTypeName(Class, LiveType));

	UCrowdyBakedRegistry* Baked = NewObject<UCrowdyBakedRegistry>(GetTransientPackage());
	TestNotNull(TEXT("baked registry created"), Baked);
	if (!Baked)
	{
		return false;
	}

	// Bake exactly the way the cook would, through the shared live->baked factory + the tag row.
	UCrowdyBakedRegistry::MakeBakedAttributes(Live, Path, Baked->ModelAttributes);
	Baked->ModelClasses.Add({ Path, LiveType });

	TestEqual(TEXT("one baked row per live attribute"), Baked->ModelAttributes.Num(), Live.Num());

	// Container tag round-trips through the asset-local finder.
	FString BakedType;
	TestTrue(TEXT("baked container tag resolves"), Baked->FindContainerTypeName(Path, BakedType));
	TestEqual(TEXT("baked container type matches live"), BakedType, LiveType);

	const TArray<FCrowdyBakedAttribute>* BakedAttrs = Baked->FindModelAttributes(Path);
	TestNotNull(TEXT("baked attributes grouped for the class"), BakedAttrs);
	if (!BakedAttrs)
	{
		return false;
	}
	TestEqual(TEXT("grouped baked count matches live"), BakedAttrs->Num(), Live.Num());

	// Every live def has a baked twin with identical fields (matched by server key, the stable id).
	for (const FCrowdyAttributeDef& L : Live)
	{
		const FCrowdyBakedAttribute* B = BakedAttrs->FindByPredicate(
			[&L](const FCrowdyBakedAttribute& A) { return A.Key == L.Key; });
		if (!TestNotNull(*FString::Printf(TEXT("baked twin for '%s'"), *L.Key), B))
		{
			continue;
		}
		TestEqual(*FString::Printf(TEXT("property name survives for '%s'"), *L.Key), B->PropertyName, L.PropertyName);
		TestEqual(*FString::Printf(TEXT("value type survives for '%s'"), *L.Key), B->ValueType, L.ValueType);
		TestEqual(*FString::Printf(TEXT("clamp flag survives for '%s'"), *L.Key), B->bHasClamp, L.bHasClamp);
		TestEqual(*FString::Printf(TEXT("clamp min survives for '%s'"), *L.Key), B->ClampMin, L.ClampMin);
		TestEqual(*FString::Printf(TEXT("clamp max survives for '%s'"), *L.Key), B->ClampMax, L.ClampMax);
		TestTrue(*FString::Printf(TEXT("onrep survives for '%s'"), *L.Key), B->OnRepFunctionName == L.OnRepFunctionName);
	}

	// Cooked-lookup fidelity: the apply path's attribute table is built from these baked rows in a cooked
	// build (by PropertyName / OnRepFunctionName) and from live metadata otherwise. Prove each row
	// re-resolves, so the cooked write-to-member + OnRep-fire path is not silently dead in a packaged build,
	// and prove the pair it would produce is the pair the live table produces for the same server key, since
	// the two constructions cannot both be exercised in one build. This fixture declares no server-key
	// override, so it cannot show what happens when one is present; that case is pinned separately by
	// CrowdySDK.GameModel.AttributeLookupKeyOverrideDivergence.
	FCrowdyModelAttributeLookup::InvalidateAll();
	for (const FCrowdyBakedAttribute& Attr : *BakedAttrs)
	{
		const FProperty* Property = Class->FindPropertyByName(Attr.PropertyName);
		TestNotNull(*FString::Printf(TEXT("baked property name '%s' re-resolves to an FProperty"), *Attr.PropertyName.ToString()), Property);

		if (Attr.OnRepFunctionName != NAME_None)
		{
			const UFunction* OnRepFn = Class->FindFunctionByName(Attr.OnRepFunctionName);
			if (TestNotNull(*FString::Printf(TEXT("baked OnRep '%s' re-resolves to a UFunction"), *Attr.OnRepFunctionName.ToString()), OnRepFn))
			{
				TestEqual(*FString::Printf(TEXT("baked OnRep '%s' is parameterless"), *Attr.OnRepFunctionName.ToString()), OnRepFn->NumParms, 0);
			}
		}

		const FCrowdyModelAttributeEntry* Entry = FCrowdyModelAttributeLookup::Find(Class, FName(*Attr.Key));
		if (TestNotNull(*FString::Printf(TEXT("server key '%s' resolves through the attribute table"), *Attr.Key), Entry))
		{
			TestTrue(*FString::Printf(TEXT("'%s' resolves the same property either way"), *Attr.Key),
				Entry->Property == Property);

			// The notify is the one field the two constructions may legitimately differ on: the bake drops a
			// notify whose signature takes parameters, while the live table carries the declared name and the
			// fire path drops it on the same parameterless check. Either they agree, or the live name is one
			// that check refuses, so nothing is ever invoked in one build and not the other.
			if (Entry->OnRepFunctionName != Attr.OnRepFunctionName)
			{
				const UFunction* Declared = Class->FindFunctionByName(Entry->OnRepFunctionName);
				TestTrue(*FString::Printf(TEXT("'%s' differs only on a notify the fire path refuses"), *Attr.Key),
					Declared == nullptr || Declared->NumParms != 0);
			}
		}
	}
	FCrowdyModelAttributeLookup::InvalidateAll();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
