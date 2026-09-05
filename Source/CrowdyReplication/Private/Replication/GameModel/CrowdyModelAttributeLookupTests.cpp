// Fill out your copyright notice in the Description page of Project Settings.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/GameModel/CrowdyAttributeRegistry.h"
#include "Replication/GameModel/CrowdyGameModelMetaKeys.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"
#include "Replication/GameModel/CrowdyModelAttributeLookup.h"
#include "Replication/State/CrowdyStateMetaKeys.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h" // FCoreUObjectDelegates, EReloadCompleteReason
#include "UObject/UnrealType.h"
#include "Utils/CrowdyBakedRegistry.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyModelAttributeLookupTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// The attribute tables are process-wide, so a test that deliberately corrupts one must leave nothing
	// behind for the next test (or for the editor) to read.
	struct FCrowdyModelAttributeLookupScope
	{
		FCrowdyModelAttributeLookupScope() { FCrowdyModelAttributeLookup::InvalidateAll(); }
		~FCrowdyModelAttributeLookupScope() { FCrowdyModelAttributeLookup::InvalidateAll(); }
	};

	// Resolves a server key the way the apply path did before the table existed: a full property walk in a
	// build with metadata, a scan of the baked rows otherwise. The reference the cache is measured against,
	// so an accidental change to the accept rules shows up as a disagreement rather than passing silently.
	FProperty* DirectResolveForServerKey(const UClass* Class, const FName ServerKey, FName& OutOnRep)
	{
		OutOnRep = NAME_None;
		if (!Class)
		{
			return nullptr;
		}
#if WITH_METADATA
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Property = *It;
			if (CrowdyGameModelMetaKeys::HasModelMeta(Property)
				&& !Property->HasMetaData(CrowdyStateMetaKeys::Replicate)
				&& !FCrowdyAttributeRegistry::MapPropertyToValueType(Property).IsEmpty()
				&& FName(*CrowdyGameModelMetaKeys::ServerKeyForProperty(Property)) == ServerKey)
			{
				const FString OnRep = Property->GetMetaData(CrowdyStateMetaKeys::OnRep);
				OutOnRep = OnRep.IsEmpty() ? NAME_None : FName(*OnRep);
				return Property;
			}
		}
		return nullptr;
#else
		if (const TArray<FCrowdyBakedAttribute>* Attrs = UCrowdyBakedRegistry::FindModelAttributes(Class))
		{
			for (const FCrowdyBakedAttribute& Attr : *Attrs)
			{
				if (FName(*Attr.Key) == ServerKey)
				{
					OutOnRep = Attr.OnRepFunctionName;
					return Class->FindPropertyByName(Attr.PropertyName);
				}
			}
		}
		return nullptr;
#endif
	}
}

// The table answers exactly what a direct resolve answers, for accepted attributes and for the keys the
// accept rules reject. The rejects matter most: a dual-marked property (it lives in exactly one plane) and an
// unsupported type must stay unresolvable, or a server write would land on a member the other plane owns.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelAttributeLookupMatchesDirectWalkTest,
	"CrowdySDK.GameModel.AttributeLookupMatchesDirectWalk", CrowdyModelAttributeLookupTestFlags)
bool FCrowdyModelAttributeLookupMatchesDirectWalkTest::RunTest(const FString& Parameters)
{
	FCrowdyModelAttributeLookupScope Scope;

	UClass* Class = UCrowdyGameModelDiscoveryTarget::StaticClass();

	// Six accepted attributes, three keys the rules reject, and one key nothing declares.
	const TCHAR* Keys[] = { TEXT("health"), TEXT("speed"), TEXT("bstunned"), TEXT("title"), TEXT("tier"),
		TEXT("badnotify"), TEXT("dualplane"), TEXT("bag"), TEXT("plainint"), TEXT("nosuchkey") };

	for (const TCHAR* KeyText : Keys)
	{
		const FName Key(KeyText);

		FName ExpectedOnRep = NAME_None;
		const FProperty* Expected = DirectResolveForServerKey(Class, Key, ExpectedOnRep);

		const FCrowdyModelAttributeEntry* Entry = FCrowdyModelAttributeLookup::Find(Class, Key);
		const FProperty* Actual = Entry ? Entry->Property : nullptr;
		const FName ActualOnRep = Entry ? Entry->OnRepFunctionName : NAME_None;

		TestTrue(*FString::Printf(TEXT("'%s' resolves to the same property"), KeyText), Actual == Expected);
		TestTrue(*FString::Printf(TEXT("'%s' resolves to the same notify"), KeyText), ActualOnRep == ExpectedOnRep);
	}

	// Spelled out so the reference walk above cannot pass by agreeing on a wrong answer.
	TestNull(TEXT("a dual-marked property is not a Server Owned attribute"),
		FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("dualplane"))));
	TestNull(TEXT("an unsupported type is not a Server Owned attribute"),
		FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("bag"))));
	TestNull(TEXT("an unmarked property is not a Server Owned attribute"),
		FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("plainint"))));

	if (const FCrowdyModelAttributeEntry* Health = FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("health"))))
	{
		TestEqual(TEXT("health resolves its declared notify"), Health->OnRepFunctionName, FName(TEXT("OnRep_Health")));
	}
	else
	{
		AddError(TEXT("health did not resolve"));
	}
	if (const FCrowdyModelAttributeEntry* Speed = FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("speed"))))
	{
		TestEqual(TEXT("an attribute with no notify resolves NAME_None"), Speed->OnRepFunctionName, FName(NAME_None));
	}

	return true;
}

// The answer really comes from a stored table rather than a fresh walk: an entry rewritten to point at a
// different member of the same class keeps being served, and only invalidation puts the true member back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelAttributeLookupServesCachedTableTest,
	"CrowdySDK.GameModel.AttributeLookupServesCachedTable", CrowdyModelAttributeLookupTestFlags)
bool FCrowdyModelAttributeLookupServesCachedTableTest::RunTest(const FString& Parameters)
{
	FCrowdyModelAttributeLookupScope Scope;

	UClass* Class = UCrowdyGameModelTestTarget::StaticClass();
	FProperty* Hp = Class->FindPropertyByName(TEXT("Hp"));
	FProperty* Mana = Class->FindPropertyByName(TEXT("Mana"));
	if (!Hp || !Mana)
	{
		AddError(TEXT("test fixture properties did not resolve"));
		return false;
	}

	const FCrowdyModelAttributeEntry* First = FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("hp")));
	if (!First)
	{
		AddError(TEXT("'hp' did not resolve on the fixture"));
		return false;
	}
	TestTrue(TEXT("'hp' resolves to Hp"), First->Property == Hp);
	TestTrue(TEXT("one class is cached"), FCrowdyModelAttributeLookup::IsClassCachedForTest(Class));

	TestTrue(TEXT("the cached entry can be rewritten"),
		FCrowdyModelAttributeLookup::OverwriteEntryForTest(Class, FName(TEXT("hp")), Mana, FName(TEXT("OnRep_Mana"))));

	const FCrowdyModelAttributeEntry* Served = FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("hp")));
	if (!Served)
	{
		AddError(TEXT("'hp' stopped resolving after the rewrite"));
		return false;
	}
	TestTrue(TEXT("the rewritten property is served, so the table is not rebuilt per call"),
		Served->Property == Mana);
	TestEqual(TEXT("the rewritten notify is served too"),
		FCrowdyModelAttributeLookup::FindOnRep(Class->GetDefaultObject(), FName(TEXT("hp"))), FName(TEXT("OnRep_Mana")));

	FCrowdyModelAttributeLookup::InvalidateClass(Class);
	TestFalse(TEXT("the class is no longer cached"), FCrowdyModelAttributeLookup::IsClassCachedForTest(Class));

	const FCrowdyModelAttributeEntry* Rebuilt = FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("hp")));
	if (!Rebuilt)
	{
		AddError(TEXT("'hp' did not resolve after invalidation"));
		return false;
	}
	TestTrue(TEXT("invalidation re-resolves the true property"), Rebuilt->Property == Hp);
	TestFalse(TEXT("the rewritten property is gone"), Rebuilt->Property == Mana);
	TestEqual(TEXT("invalidation re-resolves the true notify"), Rebuilt->OnRepFunctionName, FName(TEXT("OnRep_Hp")));

	return true;
}

// The reload-complete delegate is actually bound: broadcasting it drops every table, and the next lookup
// hands back a freshly resolved property rather than the one the table was holding.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelAttributeLookupReloadInvalidatesTest,
	"CrowdySDK.GameModel.AttributeLookupReloadInvalidates", CrowdyModelAttributeLookupTestFlags)
bool FCrowdyModelAttributeLookupReloadInvalidatesTest::RunTest(const FString& Parameters)
{
	FCrowdyModelAttributeLookupScope Scope;

	UClass* Class = UCrowdyGameModelTestTarget::StaticClass();
	FProperty* Hp = Class->FindPropertyByName(TEXT("Hp"));
	FProperty* Mana = Class->FindPropertyByName(TEXT("Mana"));
	if (!Hp || !Mana)
	{
		AddError(TEXT("test fixture properties did not resolve"));
		return false;
	}

	FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("hp")));
	TestTrue(TEXT("the class is cached before the reload"), FCrowdyModelAttributeLookup::IsClassCachedForTest(Class));
	TestTrue(TEXT("the entry can be rewritten"),
		FCrowdyModelAttributeLookup::OverwriteEntryForTest(Class, FName(TEXT("hp")), Mana, FName(TEXT("OnRep_Mana"))));

	FCoreUObjectDelegates::ReloadCompleteDelegate.Broadcast(EReloadCompleteReason::None);

	TestEqual(TEXT("every table is dropped by the reload"), FCrowdyModelAttributeLookup::NumCachedClasses(), 0);

	const FCrowdyModelAttributeEntry* Rebuilt = FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("hp")));
	if (!Rebuilt)
	{
		AddError(TEXT("'hp' did not resolve after the reload"));
		return false;
	}
	TestTrue(TEXT("the reload forces a genuine re-resolve"), Rebuilt->Property == Hp);
	TestFalse(TEXT("the pre-reload entry is not served"), Rebuilt->Property == Mana);
	TestEqual(TEXT("the notify is re-resolved too"), Rebuilt->OnRepFunctionName, FName(TEXT("OnRep_Hp")));

	return true;
}

// A key no attribute declares stays null, and asking for it repeatedly neither builds a second table nor
// rebuilds the one that exists (proved by a rewritten entry surviving the misses).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelAttributeLookupUnknownKeyTest,
	"CrowdySDK.GameModel.AttributeLookupUnknownKey", CrowdyModelAttributeLookupTestFlags)
bool FCrowdyModelAttributeLookupUnknownKeyTest::RunTest(const FString& Parameters)
{
	FCrowdyModelAttributeLookupScope Scope;

	UClass* Class = UCrowdyGameModelTestTarget::StaticClass();
	FProperty* Mana = Class->FindPropertyByName(TEXT("Mana"));
	if (!Mana)
	{
		AddError(TEXT("test fixture property did not resolve"));
		return false;
	}

	TestNull(TEXT("an undeclared key resolves to nothing"),
		FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("not_an_attribute"))));
	TestEqual(TEXT("a miss still resolves no notify"),
		FCrowdyModelAttributeLookup::FindOnRep(Class->GetDefaultObject(), FName(TEXT("not_an_attribute"))), FName(NAME_None));
	TestEqual(TEXT("one table covers the class"), FCrowdyModelAttributeLookup::NumCachedClasses(), 1);

	TestTrue(TEXT("the entry can be rewritten"),
		FCrowdyModelAttributeLookup::OverwriteEntryForTest(Class, FName(TEXT("hp")), Mana, FName(TEXT("OnRep_Mana"))));

	for (int32 Attempt = 0; Attempt < 8; ++Attempt)
	{
		TestNull(TEXT("a repeated miss stays null"),
			FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("not_an_attribute"))));
	}

	TestEqual(TEXT("repeated misses do not add tables"), FCrowdyModelAttributeLookup::NumCachedClasses(), 1);

	const FCrowdyModelAttributeEntry* Survivor = FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("hp")));
	if (!Survivor)
	{
		AddError(TEXT("'hp' stopped resolving"));
		return false;
	}
	TestTrue(TEXT("the misses did not rebuild the table"), Survivor->Property == Mana);

	return true;
}

// A subclass resolves attributes its parent declares, so its cached table points at reflection data another
// class owns. Dropping only the declaring class therefore leaves the subclass answering from properties that
// class no longer owns, which is why anything that rebuilds reflection has to drop every table rather than
// one. Pins both halves: the ancestor ownership, and the fact that a single-class drop does not reach it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelAttributeLookupInheritedPropertyTest,
	"CrowdySDK.GameModel.AttributeLookupInheritedProperty", CrowdyModelAttributeLookupTestFlags)
bool FCrowdyModelAttributeLookupInheritedPropertyTest::RunTest(const FString& Parameters)
{
	FCrowdyModelAttributeLookupScope Scope;

	UClass* Base = UCrowdyGameModelTestTarget::StaticClass();
	UClass* Derived = UCrowdyGameModelTestTargetDerived::StaticClass();

	const FCrowdyModelAttributeEntry* Entry = FCrowdyModelAttributeLookup::Find(Derived, FName(TEXT("hp")));
	if (!Entry || !Entry->Property)
	{
		AddError(TEXT("the subclass did not resolve an attribute declared by its parent"));
		return false;
	}

	TestEqual(TEXT("the subclass resolves a property its parent owns"),
		Entry->Property->GetOwnerClass(), Base);
	TestTrue(TEXT("the subclass has its own table"),
		FCrowdyModelAttributeLookup::IsClassCachedForTest(Derived));

	FCrowdyModelAttributeLookup::InvalidateClass(Base);
	TestTrue(TEXT("dropping the declaring class alone leaves the subclass table in place"),
		FCrowdyModelAttributeLookup::IsClassCachedForTest(Derived));

	FCrowdyModelAttributeLookup::InvalidateAll();
	TestFalse(TEXT("dropping every table reaches the subclass"),
		FCrowdyModelAttributeLookup::IsClassCachedForTest(Derived));

	return true;
}

// An attribute with a meta=(CrowdyKey=...) override is addressed by that key on the server, so the apply path
// has to resolve it by that key too. It once derived the key from the property name here and ignored the
// override, which meant an overridden attribute silently never applied in a build with metadata while working
// in a packaged one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelAttributeLookupKeyOverrideTest,
	"CrowdySDK.GameModel.AttributeLookupKeyOverride", CrowdyModelAttributeLookupTestFlags)
bool FCrowdyModelAttributeLookupKeyOverrideTest::RunTest(const FString& Parameters)
{
#if WITH_METADATA
	FCrowdyModelAttributeLookupScope Scope;

	UClass* Class = UCrowdyGameModelKeyVisTarget::StaticClass();
	FProperty* Reserve = Class->FindPropertyByName(TEXT("Reserve"));
	if (!Reserve)
	{
		AddError(TEXT("test fixture property did not resolve"));
		return false;
	}

	// Read the override off the fixture rather than assuming it, so the test fails loudly if the fixture stops
	// declaring one instead of quietly asserting nothing.
	const FString OverriddenKey = Reserve->GetMetaData(CrowdyGameModelMetaKeys::Key).ToLower();
	TestEqual(TEXT("the fixture declares a server key override"), OverriddenKey, FString(TEXT("ammo")));

	const FCrowdyModelAttributeEntry* Overridden = FCrowdyModelAttributeLookup::Find(Class, FName(*OverriddenKey));
	if (!Overridden)
	{
		AddError(TEXT("the apply path did not resolve the overridden server key"));
		return false;
	}
	TestTrue(TEXT("the overridden key resolves to the property that declares it"), Overridden->Property == Reserve);

	// The name the server does NOT use must not resolve, or a value could apply under either key.
	TestNull(TEXT("the property-derived key is not also live"),
		FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("reserve"))));

	// A sibling with no override still resolves by its derived name, so the override is per property.
	const FCrowdyModelAttributeEntry* Public = FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("publicscore")));
	TestNotNull(TEXT("an attribute with no override still resolves by its derived key"), Public);
#endif

	return true;
}

// Two properties can resolve to one server key once an override is in play. Discovery keeps the first it sees
// and drops the rest, so the server only ever has the first one; the apply path has to pick the same property
// or a value would be written onto a member the server does not believe it is addressing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyModelAttributeLookupDuplicateKeyTest,
	"CrowdySDK.GameModel.AttributeLookupDuplicateKey", CrowdyModelAttributeLookupTestFlags)
bool FCrowdyModelAttributeLookupDuplicateKeyTest::RunTest(const FString& Parameters)
{
#if WITH_METADATA
	FCrowdyModelAttributeLookupScope Scope;

	UClass* Class = UCrowdyGameModelDupKeyTarget::StaticClass();
	FProperty* Ammo = Class->FindPropertyByName(TEXT("Ammo"));
	FProperty* Reserve = Class->FindPropertyByName(TEXT("Reserve"));
	if (!Ammo || !Reserve)
	{
		AddError(TEXT("test fixture properties did not resolve"));
		return false;
	}

	// Discovery reports the collision at authoring time; the lookup resolves silently, so whitelist nothing.
	const FCrowdyModelAttributeEntry* Entry = FCrowdyModelAttributeLookup::Find(Class, FName(TEXT("ammo")));
	if (!Entry)
	{
		AddError(TEXT("the colliding server key resolved to nothing"));
		return false;
	}

	// Field iteration reaches Ammo first, which is the one discovery keeps.
	TestTrue(TEXT("the colliding key resolves to the first declared property"), Entry->Property == Ammo);
	TestFalse(TEXT("the dropped duplicate does not win"), Entry->Property == Reserve);
#endif

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
