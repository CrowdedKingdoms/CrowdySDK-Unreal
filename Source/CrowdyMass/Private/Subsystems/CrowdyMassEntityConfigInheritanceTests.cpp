#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "MassEntityConfigAsset.h"
#include "Misc/AutomationTest.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Which per-class Mass entity config binding answers for a class that has none of its own. A binding is
// inherited down the class chain, so the cases that matter are the ones a chain can get wrong: a subclass
// finding its base's row at all, two bound classes in one chain resolving to the nearer of them, and a
// class in a different branch finding nothing rather than the first row in the list.
//
// Nothing here needs a world. Resolution reads the flattened binding map and the class chain, and touches
// neither the entity manager nor the router.
//
// Enum results are compared as bytes, matching the other subsystem cases: the automation framework's
// generic comparison wants a way to print the value, which a plain enum does not have.
static constexpr EAutomationTestFlags CrowdyMassEntityConfigInheritanceTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

struct FCrowdyMassEntityConfigInheritanceTestFixture
{
	static UCrowdyMassEntitySubsystem* MakeSubsystem()
	{
		UCrowdyMassEntitySubsystem* Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());

		// Installs the suppression with no binding, so a class nothing else configures resolves to nothing
		// here rather than to whatever the running project happens to ship.
		Subsystem->SetAuthoredEntityConfigForTest(nullptr, nullptr);
		return Subsystem;
	}

	/**
	 * Stands in for a COMPILED Blueprint class, which is what a resolve is asked about at runtime: it carries
	 * the parent's constructor, is linked against that parent so it has the parent's size, and then has a
	 * default object built. Building a default object before the link allocates a zero-sized object and runs
	 * an actor's constructor over it, which corrupts the heap silently.
	 *
	 * ParentClass is a parameter because these cases are about the chain rather than about one class, and a
	 * Blueprint deriving from another Blueprint is the shape an author actually authors.
	 */
	static UBlueprintGeneratedClass* MakeCompiledActorClass(UClass* ParentClass)
	{
		UBlueprintGeneratedClass* GeneratedClass =
			NewObject<UBlueprintGeneratedClass>(GetTransientPackage(), NAME_None, RF_Transient);
		GeneratedClass->ClassWithin = ParentClass->ClassWithin;
		GeneratedClass->ClassConstructor = ParentClass->ClassConstructor;
		GeneratedClass->ClassCastFlags |= ParentClass->ClassCastFlags;
		GeneratedClass->SetSuperStruct(ParentClass);
		GeneratedClass->Bind();
		GeneratedClass->StaticLink(/*bRelinkExistingProperties=*/true);
		GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/true);
		return GeneratedClass;
	}

	// Nothing here reads a config's contents, only which one came back, so an empty asset is enough and
	// pointer identity is the whole assertion.
	static UMassEntityConfigAsset* MakeConfig()
	{
		return NewObject<UMassEntityConfigAsset>(GetTransientPackage(), NAME_None, RF_Transient);
	}
};

// The change E2 makes, stated as the case that used to fail: a subclass of a bound class resolves to the
// base's config instead of falling past every per-class row to the backend-wide default.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityConfigInheritedFromBaseTest,
	"CrowdySDK.Mass.EntityConfigBindingIsInheritedBySubclasses", CrowdyMassEntityConfigInheritanceTestFlags)
bool FCrowdyMassEntityConfigInheritedFromBaseTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UBlueprintGeneratedClass> BaseClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> DerivedClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(BaseClass.Get()));

	const TStrongObjectPtr<UMassEntityConfigAsset> BaseConfig(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());
	Subsystem->SetAuthoredEntityConfigForTest(BaseClass.Get(), BaseConfig.Get());

	ECrowdyMassEntityConfigSource ExactSource = ECrowdyMassEntityConfigSource::None;
	TestSamePtr(TEXT("the bound class itself still resolves to its own config"),
		Subsystem->FindAuthoredEntityConfig(BaseClass.Get(), ExactSource),
		static_cast<const UMassEntityConfigAsset*>(BaseConfig.Get()));
	TestEqual(TEXT("and reports the binding as an exact one"),
		static_cast<uint8>(ExactSource), static_cast<uint8>(ECrowdyMassEntityConfigSource::PerClassBinding));

	ECrowdyMassEntityConfigSource DerivedSource = ECrowdyMassEntityConfigSource::None;
	TestSamePtr(TEXT("a subclass with no binding of its own resolves to the base's config"),
		Subsystem->FindAuthoredEntityConfig(DerivedClass.Get(), DerivedSource),
		static_cast<const UMassEntityConfigAsset*>(BaseConfig.Get()));

	// The two sources are separate answers, not decoration: this is what stops a report telling an author to
	// go and find a row for their class in a list that has never named it.
	TestEqual(TEXT("and reports it as inherited rather than as a row naming that subclass"),
		static_cast<uint8>(DerivedSource),
		static_cast<uint8>(ECrowdyMassEntityConfigSource::InheritedClassBinding));
	TestSamePtr(TEXT("and names the class carrying the row an author would have to edit"),
		Subsystem->FindEntityConfigBindingClass(DerivedClass.Get()),
		static_cast<const UClass*>(BaseClass.Get()));

	return true;
}

// Two bound classes in one chain. The nearer one has to win, because the whole point of binding a subclass
// is to say it differs from its base, and a walk that answered with the first row it found in the list, or
// with the topmost binding, would silently ignore that.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityConfigMostDerivedWinsTest,
	"CrowdySDK.Mass.EntityConfigBindingPrefersTheNearestBoundAncestor", CrowdyMassEntityConfigInheritanceTestFlags)
bool FCrowdyMassEntityConfigMostDerivedWinsTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UBlueprintGeneratedClass> BaseClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> MiddleClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(BaseClass.Get()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> LeafClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(MiddleClass.Get()));

	const TStrongObjectPtr<UMassEntityConfigAsset> BaseConfig(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());
	const TStrongObjectPtr<UMassEntityConfigAsset> MiddleConfig(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());

	// Bound base-first, so a walk that stopped at the last row written rather than at the nearest class
	// would answer with the base and look right.
	Subsystem->SetAuthoredEntityConfigForTest(BaseClass.Get(), BaseConfig.Get());
	Subsystem->SetAuthoredEntityConfigForTest(MiddleClass.Get(), MiddleConfig.Get());

	ECrowdyMassEntityConfigSource LeafSource = ECrowdyMassEntityConfigSource::None;
	TestSamePtr(TEXT("the leaf resolves to the nearest bound ancestor's config, not the topmost one's"),
		Subsystem->FindAuthoredEntityConfig(LeafClass.Get(), LeafSource),
		static_cast<const UMassEntityConfigAsset*>(MiddleConfig.Get()));
	TestEqual(TEXT("reported as inherited"),
		static_cast<uint8>(LeafSource),
		static_cast<uint8>(ECrowdyMassEntityConfigSource::InheritedClassBinding));
	TestSamePtr(TEXT("and names the nearest bound ancestor"),
		Subsystem->FindEntityConfigBindingClass(LeafClass.Get()),
		static_cast<const UClass*>(MiddleClass.Get()));

	// The classes that ARE bound keep answering exactly, so widening the lookup did not turn an exact row
	// into an inherited one for the class that owns it.
	ECrowdyMassEntityConfigSource MiddleSource = ECrowdyMassEntityConfigSource::None;
	TestSamePtr(TEXT("the middle class still resolves to its own config"),
		Subsystem->FindAuthoredEntityConfig(MiddleClass.Get(), MiddleSource),
		static_cast<const UMassEntityConfigAsset*>(MiddleConfig.Get()));
	TestEqual(TEXT("as an exact binding"),
		static_cast<uint8>(MiddleSource),
		static_cast<uint8>(ECrowdyMassEntityConfigSource::PerClassBinding));

	ECrowdyMassEntityConfigSource BaseSource = ECrowdyMassEntityConfigSource::None;
	TestSamePtr(TEXT("and the base still resolves to its own, unaffected by the row below it"),
		Subsystem->FindAuthoredEntityConfig(BaseClass.Get(), BaseSource),
		static_cast<const UMassEntityConfigAsset*>(BaseConfig.Get()));
	TestEqual(TEXT("also as an exact binding"),
		static_cast<uint8>(BaseSource),
		static_cast<uint8>(ECrowdyMassEntityConfigSource::PerClassBinding));

	return true;
}

// The bound on the change. Inheritance walks UP and only up, so a class in a different branch of the tree
// finds nothing, and a bound class's own PARENT is not covered by its child's row.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityConfigDoesNotSpreadSidewaysTest,
	"CrowdySDK.Mass.EntityConfigBindingDoesNotSpreadOffTheChain", CrowdyMassEntityConfigInheritanceTestFlags)
bool FCrowdyMassEntityConfigDoesNotSpreadSidewaysTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UBlueprintGeneratedClass> ParentClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> BoundChildClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(ParentClass.Get()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> SiblingClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));

	const TStrongObjectPtr<UMassEntityConfigAsset> ChildConfig(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());
	Subsystem->SetAuthoredEntityConfigForTest(BoundChildClass.Get(), ChildConfig.Get());

	ECrowdyMassEntityConfigSource ParentSource = ECrowdyMassEntityConfigSource::PerClassBinding;
	TestNull(TEXT("a bound class's parent is not covered by its child's row"),
		Subsystem->FindAuthoredEntityConfig(ParentClass.Get(), ParentSource));
	TestEqual(TEXT("and reports nothing authored, so a spawn of it would derive"),
		static_cast<uint8>(ParentSource), static_cast<uint8>(ECrowdyMassEntityConfigSource::None));

	ECrowdyMassEntityConfigSource SiblingSource = ECrowdyMassEntityConfigSource::PerClassBinding;
	TestNull(TEXT("and a class in another branch finds nothing rather than the only row in the list"),
		Subsystem->FindAuthoredEntityConfig(SiblingClass.Get(), SiblingSource));
	TestEqual(TEXT("also reported as nothing authored"),
		static_cast<uint8>(SiblingSource), static_cast<uint8>(ECrowdyMassEntityConfigSource::None));

	TestNull(TEXT("and no class carries a row for it"),
		Subsystem->FindEntityConfigBindingClass(SiblingClass.Get()));

	return true;
}

// The remedy an author is handed when an authored config overrides something their class declares. The
// old wording was one sentence, "stop binding the config to this class", and it is followable in exactly
// one of the four ways a config can answer. The other three send an author looking for a row that is not
// there, which ends with them deciding the warning is wrong rather than finding the fix.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityConfigRemedyNamesTheRealRowTest,
	"CrowdySDK.Mass.EntityConfigRemedyNamesTheRowThatActuallyExists", CrowdyMassEntityConfigInheritanceTestFlags)
bool FCrowdyMassEntityConfigRemedyNamesTheRealRowTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UBlueprintGeneratedClass> BaseClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> DerivedClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(BaseClass.Get()));

	const TStrongObjectPtr<UMassEntityConfigAsset> BaseConfig(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());
	Subsystem->SetAuthoredEntityConfigForTest(BaseClass.Get(), BaseConfig.Get());

	// The one case the old sentence was right about: a row names this exact class, so deleting it is the fix.
	const FString ExactRemedy = Subsystem->DescribeAuthoredConfigRemedy(BaseClass.Get());
	TestTrue(TEXT("a class with its own row is told to remove that row"),
		ExactRemedy.Contains(TEXT("remove the row naming this class")));

	// The case E2 created, and the one the old sentence turned into a wrong instruction.
	const FString InheritedRemedy = Subsystem->DescribeAuthoredConfigRemedy(DerivedClass.Get());
	TestFalse(TEXT("a class with no row of its own is NOT told to remove one"),
		InheritedRemedy.Contains(TEXT("remove the row naming this class")));
	TestTrue(TEXT("it is told nothing binds it"),
		InheritedRemedy.Contains(TEXT("Nothing binds this class")));

	// Naming the ancestor is the whole point: without it the author has the entire parent chain to search
	// by hand, which is the difficulty an inherited binding creates.
	TestTrue(TEXT("and the ancestor carrying the row is named"),
		InheritedRemedy.Contains(BaseClass->GetName()));
	TestTrue(TEXT("and the author is warned that deleting it moves every class beneath it"),
		InheritedRemedy.Contains(TEXT("every class beneath it")));

	return true;
}

// The two routes that were never per-class rows at all. These were wrong before E2 as well, so this is
// not a regression being repaired but a remedy that never worked for them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityConfigRemedyHandlesNonRowSourcesTest,
	"CrowdySDK.Mass.EntityConfigRemedyHandlesSourcesThatAreNotRows", CrowdyMassEntityConfigInheritanceTestFlags)
bool FCrowdyMassEntityConfigRemedyHandlesNonRowSourcesTest::RunTest(const FString& Parameters)
{
	// Deliberately NOT the suppressed fixture. The suppression seam returns before the backend override is
	// consulted, which is the branch this case is about, so installing it here would leave the case
	// passing against the None branch instead and prove nothing about the override.
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> UnboundClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));
	const TStrongObjectPtr<UMassEntityConfigAsset> OverrideConfig(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());

	// The backend's own override, which answers for every class it does not bind individually.
	Subsystem->ConfigureOverrides(TSoftObjectPtr<UMassEntityConfigAsset>(OverrideConfig.Get()), -1.0f);

	const FString OverrideRemedy = Subsystem->DescribeAuthoredConfigRemedy(UnboundClass.Get());
	TestFalse(TEXT("a class answered by the backend override is not told to remove a row"),
		OverrideRemedy.Contains(TEXT("remove the row naming this class")));
	TestTrue(TEXT("it is pointed at the override that actually answered"),
		OverrideRemedy.Contains(TEXT("Remote Entity Config Override")));
	TestTrue(TEXT("and told to give the class a row of its own instead"),
		OverrideRemedy.Contains(TEXT("give this class its own row")));

	return true;
}

#if WITH_EDITOR

// Recompiling a Blueprint in PIE mints a new class. The binding has to follow it, or every entity of that
// class is built from whatever the walk finds further up the chain for the rest of the session.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityConfigFollowsAReplacedClassTest,
	"CrowdySDK.Mass.EntityConfigBindingFollowsAReplacedClass", CrowdyMassEntityConfigInheritanceTestFlags)
bool FCrowdyMassEntityConfigFollowsAReplacedClassTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UBlueprintGeneratedClass> OldClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> NewClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));

	const TStrongObjectPtr<UMassEntityConfigAsset> Config(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());
	Subsystem->SetAuthoredEntityConfigForTest(OldClass.Get(), Config.Get());

	TMap<UObject*, UObject*> ReplacementMap;
	ReplacementMap.Add(OldClass.Get(), NewClass.Get());
	Subsystem->HandleObjectsReplacedForTest(ReplacementMap);

	TestEqual(TEXT("the binding survives the replacement rather than being dropped"),
		Subsystem->NumEntityConfigBindingsForTest(), 1);
	TestSamePtr(TEXT("and is now filed under the replacement class"),
		Subsystem->FindExactEntityConfigBindingForTest(NewClass.Get()),
		static_cast<const UMassEntityConfigAsset*>(Config.Get()));
	TestNull(TEXT("and no longer under the class that was replaced"),
		Subsystem->FindExactEntityConfigBindingForTest(OldClass.Get()));

	ECrowdyMassEntityConfigSource Source = ECrowdyMassEntityConfigSource::None;
	TestSamePtr(TEXT("so an entity of the recompiled class still resolves its own config"),
		Subsystem->FindAuthoredEntityConfig(NewClass.Get(), Source),
		static_cast<const UMassEntityConfigAsset*>(Config.Get()));
	TestEqual(TEXT("as an exact row rather than an inherited one"),
		static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassEntityConfigSource::PerClassBinding));

	return true;
}

// The other half of "remap, never clear": nothing rebuilds these bindings except a backend activation, so a
// class the replacement map does not name has to come out of the hook exactly as it went in.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityConfigSurvivesAnUnrelatedReplacementTest,
	"CrowdySDK.Mass.EntityConfigBindingSurvivesAnUnrelatedReplacement", CrowdyMassEntityConfigInheritanceTestFlags)
bool FCrowdyMassEntityConfigSurvivesAnUnrelatedReplacementTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UBlueprintGeneratedClass> BoundClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> UnrelatedOld(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> UnrelatedNew(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));

	const TStrongObjectPtr<UMassEntityConfigAsset> Config(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());
	Subsystem->SetAuthoredEntityConfigForTest(BoundClass.Get(), Config.Get());

	TMap<UObject*, UObject*> ReplacementMap;
	ReplacementMap.Add(UnrelatedOld.Get(), UnrelatedNew.Get());
	Subsystem->HandleObjectsReplacedForTest(ReplacementMap);

	TestSamePtr(TEXT("a binding nothing replaced is still filed under its own class"),
		Subsystem->FindExactEntityConfigBindingForTest(BoundClass.Get()),
		static_cast<const UMassEntityConfigAsset*>(Config.Get()));

	return true;
}

// The config asset is reinstanced by the same hook when it is a Blueprint-backed one, and a row left naming
// the old asset builds every entity of that class from storage the engine has finished with.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityConfigFollowsAReplacedAssetTest,
	"CrowdySDK.Mass.EntityConfigBindingFollowsAReplacedConfigAsset", CrowdyMassEntityConfigInheritanceTestFlags)
bool FCrowdyMassEntityConfigFollowsAReplacedAssetTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UBlueprintGeneratedClass> BoundClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));

	const TStrongObjectPtr<UMassEntityConfigAsset> OldConfig(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());
	const TStrongObjectPtr<UMassEntityConfigAsset> NewConfig(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());
	Subsystem->SetAuthoredEntityConfigForTest(BoundClass.Get(), OldConfig.Get());

	TMap<UObject*, UObject*> ReplacementMap;
	ReplacementMap.Add(OldConfig.Get(), NewConfig.Get());
	Subsystem->HandleObjectsReplacedForTest(ReplacementMap);

	TestSamePtr(TEXT("the row now names the replacement asset"),
		Subsystem->FindExactEntityConfigBindingForTest(BoundClass.Get()),
		static_cast<const UMassEntityConfigAsset*>(NewConfig.Get()));

	return true;
}

// The hook fires once per replaced INSTANCE and each broadcast names one pair, so a bound class with no
// instance in the world is never named by any of them. Its row cannot be remapped and must not be kept: a
// strong key on a trashed class holds that class and its default object alive, and an entity of the
// replacement class would inherit some ancestor's row instead of finding its own.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityConfigDropsAStaleKeyTest,
	"CrowdySDK.Mass.EntityConfigBindingDropsAKeyNamingAReplacedClass", CrowdyMassEntityConfigInheritanceTestFlags)
bool FCrowdyMassEntityConfigDropsAStaleKeyTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UBlueprintGeneratedClass> StaleClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));
	const TStrongObjectPtr<UBlueprintGeneratedClass> LiveClass(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeCompiledActorClass(AActor::StaticClass()));

	const TStrongObjectPtr<UMassEntityConfigAsset> StaleConfig(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());
	const TStrongObjectPtr<UMassEntityConfigAsset> LiveConfig(
		FCrowdyMassEntityConfigInheritanceTestFixture::MakeConfig());
	Subsystem->SetAuthoredEntityConfigForTest(StaleClass.Get(), StaleConfig.Get());
	Subsystem->SetAuthoredEntityConfigForTest(LiveClass.Get(), LiveConfig.Get());

	// What reinstancing leaves behind on a class it has replaced.
	StaleClass->ClassFlags |= CLASS_NewerVersionExists;

	Subsystem->HandleObjectsReplacedForTest(TMap<UObject*, UObject*>());

	TestNull(TEXT("the row keyed on a replaced class is gone"),
		Subsystem->FindExactEntityConfigBindingForTest(StaleClass.Get()));
	TestSamePtr(TEXT("while a row keyed on a live class is untouched"),
		Subsystem->FindExactEntityConfigBindingForTest(LiveClass.Get()),
		static_cast<const UMassEntityConfigAsset*>(LiveConfig.Get()));
	TestEqual(TEXT("so exactly one row went"), Subsystem->NumEntityConfigBindingsForTest(), 1);

	return true;
}

#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
