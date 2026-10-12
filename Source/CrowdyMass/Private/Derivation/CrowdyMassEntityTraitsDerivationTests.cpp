#include "Derivation/CrowdyMassEntityTraitsDerivation.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/ActorComponent.h"
#include "Components/CrowdyMassEntityTraitsComponent.h"
#include "Derivation/CrowdyMassEntityTraitsTestTypes.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "MassEntityConfigAsset.h"
#include "MassMovementFragments.h"
#include "MassRepresentationTypes.h"
#include "MassVisualizationTrait.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// A class declares Mass traits for itself the way it declares everything else about its crowd: on a
// component, read off the class before anything spawns. The cases here ask what that reading answers, and
// they insist on the three states an author can leave it in staying three states: no component, a component
// with an empty list, and a component whose rows were never filled in. Those have three different remedies
// and would otherwise arrive as one indistinguishable "nothing".
//
// The route built here is the one a game actually uses: a component added in the Blueprint editor, which
// never reaches a class default object and is the route a default-object read alone would miss.
static constexpr EAutomationTestFlags CrowdyMassEntityTraitsDerivationTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

struct FCrowdyMassEntityTraitsDerivationTestFixture
{
	/**
	 * Stands in for a COMPILED Blueprint class, which is the only kind derivation answers for: it carries
	 * the parent's constructor, is linked against that parent so it has the parent's size, and then has a
	 * default object built. Building a default object before the link allocates a zero-sized object and
	 * runs an actor's constructor over it, which corrupts the heap silently.
	 */
	static UBlueprintGeneratedClass* MakeCompiledActorClass()
	{
		UBlueprintGeneratedClass* GeneratedClass = MakeUnfinishedActorClass();
		GeneratedClass->Bind();
		GeneratedClass->StaticLink(/*bRelinkExistingProperties=*/true);
		GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/true);
		return GeneratedClass;
	}

	/**
	 * The same class one step earlier: named a parent, not linked, and with no default object built.
	 *
	 * This is what a class still being compiled looks like from the outside, and it must never have a default
	 * object built for it here. Deliberately never linked, so a case that accidentally built one would crash
	 * rather than quietly pass.
	 */
	static UBlueprintGeneratedClass* MakeUnfinishedActorClass()
	{
		UClass* ParentClass = AActor::StaticClass();

		UBlueprintGeneratedClass* GeneratedClass =
			NewObject<UBlueprintGeneratedClass>(GetTransientPackage(), NAME_None, RF_Transient);
		GeneratedClass->ClassWithin = ParentClass->ClassWithin;
		GeneratedClass->ClassConstructor = ParentClass->ClassConstructor;
		GeneratedClass->ClassCastFlags |= ParentClass->ClassCastFlags;
		GeneratedClass->SetSuperStruct(ParentClass);
		return GeneratedClass;
	}

	// The shape any component added in the Blueprint editor takes: a construction script node holding a
	// component template, with no matching component anywhere on a default object.
	static void AddComponentNode(UBlueprintGeneratedClass* GeneratedClass, UActorComponent* Template)
	{
		if (!GeneratedClass->SimpleConstructionScript)
		{
			GeneratedClass->SimpleConstructionScript =
				NewObject<USimpleConstructionScript>(GeneratedClass, NAME_None, RF_Transient);
		}

		USCS_Node* Node = NewObject<USCS_Node>(GeneratedClass->SimpleConstructionScript, NAME_None, RF_Transient);
		Node->ComponentClass = Template->GetClass();
		Node->ComponentTemplate = Template;
		GeneratedClass->SimpleConstructionScript->AddNode(Node);
	}

	/**
	 * A traits component on the class, listing MarkerValues in order, with a null row wherever the value is
	 * negative. A negative marker stands for a row an author added and never picked a class on.
	 */
	static UCrowdyMassEntityTraitsComponent* AddTraitsComponent(UBlueprintGeneratedClass* GeneratedClass,
		const TArray<int32>& MarkerValues)
	{
		UCrowdyMassEntityTraitsComponent* Template =
			NewObject<UCrowdyMassEntityTraitsComponent>(GeneratedClass, NAME_None, RF_Transient);

		for (const int32 Marker : MarkerValues)
		{
			if (Marker < 0)
			{
				Template->Traits.Add(nullptr);
				continue;
			}

			UCrowdyMassEntityTraitsTestTrait* Trait =
				NewObject<UCrowdyMassEntityTraitsTestTrait>(Template, NAME_None, RF_Transient);
			Trait->Marker = Marker;
			Template->Traits.Add(Trait);
		}

		AddComponentNode(GeneratedClass, Template);
		return Template;
	}

	static UMassEntityConfigAsset* MakeConfig()
	{
		return NewObject<UMassEntityConfigAsset>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	template <typename TraitType>
	static TraitType* AddTraitToConfig(UMassEntityConfigAsset* Config, const EProcessorExecutionFlags Targets)
	{
		TraitType* Trait = NewObject<TraitType>(Config, NAME_None, RF_Transient);
		Trait->ValidTargetConfig = Targets;
		Config->GetMutableConfig().AddTrait(*Trait);
		return Trait;
	}

	template <typename TraitType>
	static TraitType* MakeLooseTrait(const EProcessorExecutionFlags Targets)
	{
		TraitType* Trait = NewObject<TraitType>(GetTransientPackage(), NAME_None, RF_Transient);
		Trait->ValidTargetConfig = Targets;
		return Trait;
	}
};

/**
 * A trait listed on a component added in the Blueprint editor is found, with the values authored on it.
 *
 * That route is the whole point: a component added in the Blueprint editor exists only as a construction
 * script template and never reaches a class default object, so a derivation that read the default object
 * would answer "this class declares nothing" for the way every author actually declares it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityTraitsDerivationBlueprintRouteTest,
	"CrowdySDK.Mass.EntityTraitsDerivationReadsATraitDeclaredInABlueprint",
	CrowdyMassEntityTraitsDerivationTestFlags)
bool FCrowdyMassEntityTraitsDerivationBlueprintRouteTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UBlueprintGeneratedClass> DeclaringClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeCompiledActorClass());
	FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitsComponent(DeclaringClass.Get(), {7});

	const FCrowdyMassDerivedEntityTraits Declared =
		FCrowdyMassEntityTraitsDerivation::FromClass(DeclaringClass.Get());

	if (!TestTrue(TEXT("a class that lists a trait answers with one"), Declared.IsValid()))
	{
		return false;
	}

	TestEqual(TEXT("and with exactly the one it listed"), Declared.Traits.Num(), 1);
	TestTrue(TEXT("the component it was read off was seen"), Declared.bSawComponent);
	TestEqual(TEXT("no row was left unset"), Declared.EmptyEntryCount, 0);
	TestEqual(TEXT("and no further component was shadowed"), Declared.ShadowedComponentCount, 0);

	const UCrowdyMassEntityTraitsTestTrait* Trait = Cast<UCrowdyMassEntityTraitsTestTrait>(Declared.Traits[0]);
	if (!TestNotNull(TEXT("the trait that came back is the class that was listed"), Trait))
	{
		return false;
	}

	TestEqual(TEXT("and it is the instance the component owns, carrying what was authored on it"),
		Trait->Marker, 7);
	TestEqual(TEXT("it is reported as having been added in the Blueprint, which is the route it took"),
		static_cast<int32>(Declared.Route), static_cast<int32>(ECrowdyMassEntityTraitsRoute::BlueprintComponent));
	TestTrue(TEXT("an answer that succeeded has nothing to explain"), Declared.Reason.IsEmpty());
	TestFalse(TEXT("and names the component it was read off"), Declared.ComponentName.IsEmpty());

	return true;
}

/**
 * A class that has no default object yet is refused, and is refused for that reason rather than reported as
 * declaring nothing.
 *
 * The refusal is not a nicety. The walk below it reads a class default object and WOULD build one, and
 * building one for a class still being compiled runs a constructor over an object that has no room for it.
 * The reason has to say so, because "still loading" and "declares nothing" are the same empty answer and
 * only one of them means ask again later.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityTraitsDerivationNoDefaultObjectTest,
	"CrowdySDK.Mass.EntityTraitsDerivationRefusesAClassWithNoDefaultObject",
	CrowdyMassEntityTraitsDerivationTestFlags)
bool FCrowdyMassEntityTraitsDerivationNoDefaultObjectTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UBlueprintGeneratedClass> UnfinishedClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeUnfinishedActorClass());

	const FCrowdyMassDerivedEntityTraits Declared =
		FCrowdyMassEntityTraitsDerivation::FromClass(UnfinishedClass.Get());

	TestFalse(TEXT("a class that is still forming declares nothing yet"), Declared.IsValid());
	TestTrue(TEXT("and says it is the missing default object, not a missing component"),
		Declared.Reason.Contains(TEXT("no class default object")));
	TestNull(TEXT("and nothing built a default object for it on the way past"),
		UnfinishedClass->GetDefaultObject(/*bCreateIfNeeded=*/false));

	return true;
}

/**
 * A component with an empty list and no component at all are two different messages.
 *
 * They are the same "nothing was added" from the outside, and the remedies are opposites: one is a list to
 * fill in and the other is a component to add. A class with no component is also the ordinary case for every
 * class in a project, which is why only one of the two is ever said out loud.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityTraitsDerivationEmptyComponentTest,
	"CrowdySDK.Mass.EntityTraitsDerivationTellsAnEmptyComponentFromNoComponent",
	CrowdyMassEntityTraitsDerivationTestFlags)
bool FCrowdyMassEntityTraitsDerivationEmptyComponentTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UBlueprintGeneratedClass> BareClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeCompiledActorClass());

	const FCrowdyMassDerivedEntityTraits Bare = FCrowdyMassEntityTraitsDerivation::FromClass(BareClass.Get());
	TestFalse(TEXT("a class with no such component declares nothing"), Bare.IsValid());
	TestFalse(TEXT("and no component was seen for it"), Bare.bSawComponent);
	TestTrue(TEXT("so the advice is to add one"), Bare.Reason.Contains(TEXT("no Crowdy Mass Entity Traits component")));

	const TStrongObjectPtr<UBlueprintGeneratedClass> EmptyClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeCompiledActorClass());
	FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitsComponent(EmptyClass.Get(), {});

	const FCrowdyMassDerivedEntityTraits Empty = FCrowdyMassEntityTraitsDerivation::FromClass(EmptyClass.Get());
	TestFalse(TEXT("a class whose component lists nothing also declares nothing"), Empty.IsValid());
	TestTrue(TEXT("but the component WAS seen, which is what separates the two"), Empty.bSawComponent);
	TestEqual(TEXT("and no row was left unset, which is the third state again"), Empty.EmptyEntryCount, 0);
	TestTrue(TEXT("so the advice is to fill the list in or remove the component"),
		Empty.Reason.Contains(TEXT("empty list")));

	return true;
}

/**
 * A row left set to none is skipped, counted, and reported as its own mistake.
 *
 * Counting it is what makes it a third state rather than a quieter version of an empty list: the remedy is
 * one row to pick a class on, not a list to populate. Skipping it is not optional either, because a null
 * reaching the config's trait array would be a null the engine walks past on every build.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityTraitsDerivationUnsetRowTest,
	"CrowdySDK.Mass.EntityTraitsDerivationSkipsAndCountsARowLeftUnset",
	CrowdyMassEntityTraitsDerivationTestFlags)
bool FCrowdyMassEntityTraitsDerivationUnsetRowTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UBlueprintGeneratedClass> MixedClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeCompiledActorClass());
	FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitsComponent(MixedClass.Get(), {-1, 4});

	const FCrowdyMassDerivedEntityTraits Mixed = FCrowdyMassEntityTraitsDerivation::FromClass(MixedClass.Get());

	if (!TestEqual(TEXT("the row that was filled in still answers"), Mixed.Traits.Num(), 1))
	{
		return false;
	}

	TestNotNull(TEXT("and nothing null reached the answer"), Mixed.Traits[0]);
	TestEqual(TEXT("the unset row was counted rather than silently dropped"), Mixed.EmptyEntryCount, 1);
	TestTrue(TEXT("a list with one usable row still succeeds, so it has nothing to explain"),
		Mixed.Reason.IsEmpty());

	const TStrongObjectPtr<UBlueprintGeneratedClass> UnsetClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeCompiledActorClass());
	FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitsComponent(UnsetClass.Get(), {-1, -1});

	const FCrowdyMassDerivedEntityTraits Unset = FCrowdyMassEntityTraitsDerivation::FromClass(UnsetClass.Get());
	TestFalse(TEXT("a list of nothing but unset rows declares nothing"), Unset.IsValid());
	TestEqual(TEXT("and every one of them was counted"), Unset.EmptyEntryCount, 2);
	TestTrue(TEXT("so the advice names the rows rather than the list"),
		Unset.Reason.Contains(TEXT("set to none")));

	return true;
}

/**
 * A component that lists nothing is walked past, and does not shadow one that lists something.
 *
 * The components do not arrive most-derived first: every one declared in C++ is handed back before any added
 * in a Blueprint. So a base class that creates this component to expose the slot, and leaves a derived class
 * to fill it in, is the ordinary arrangement rather than an odd one, and a rule of "the first component seen
 * answers" hands back the empty list every time. The author is then told their list is empty while looking
 * at the one they filled in, which is advice they cannot act on.
 *
 * The second half keeps the shadow count honest: two components that BOTH list something still have no order
 * worth relying on, so one answers and the rest are counted and reported.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityTraitsEmptyComponentDoesNotShadowTest,
	"CrowdySDK.Mass.EntityTraitsAnEmptyComponentDoesNotShadowAFilledOne",
	CrowdyMassEntityTraitsDerivationTestFlags)
bool FCrowdyMassEntityTraitsEmptyComponentDoesNotShadowTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UBlueprintGeneratedClass> ShadowedClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeCompiledActorClass());

	// The empty one first, which is the order the engine's own walk produces for a C++ base plus a Blueprint
	// child, and the order in which a first-wins rule loses the filled-in list.
	FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitsComponent(ShadowedClass.Get(), {});
	const UCrowdyMassEntityTraitsComponent* const Filled =
		FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitsComponent(ShadowedClass.Get(), {7});

	const FCrowdyMassDerivedEntityTraits Shadowed =
		FCrowdyMassEntityTraitsDerivation::FromClass(ShadowedClass.Get());

	if (!TestEqual(TEXT("the component that lists something is the one that answers"), Shadowed.Traits.Num(), 1))
	{
		return false;
	}

	TestTrue(TEXT("and it is that component's trait, not an empty answer from the one before it"),
		Shadowed.IsValid());
	TestEqual(TEXT("the empty one is not counted as shadowing anything, because it lost nothing"),
		Shadowed.ShadowedComponentCount, 0);
	TestTrue(TEXT("so there is nothing to explain and no empty-list advice is built"),
		Shadowed.Reason.IsEmpty());

	// The name is what every message about this class prints, so an author sent to the component that
	// answered has to be sent to the one holding the trait rather than to the empty one walked past.
	TestEqual(TEXT("and the component named is the one whose trait was taken"),
		Shadowed.ComponentName, Filled->GetName());

	// Two lists that both say something is the case the count exists for.
	const TStrongObjectPtr<UBlueprintGeneratedClass> TwoFilledClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeCompiledActorClass());
	FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitsComponent(TwoFilledClass.Get(), {1});
	FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitsComponent(TwoFilledClass.Get(), {2});

	const FCrowdyMassDerivedEntityTraits TwoFilled =
		FCrowdyMassEntityTraitsDerivation::FromClass(TwoFilledClass.Get());

	TestEqual(TEXT("only one of two filled lists answers"), TwoFilled.Traits.Num(), 1);
	TestEqual(TEXT("and the one that did not is counted, so it can be reported"),
		TwoFilled.ShadowedComponentCount, 1);

	return true;
}

/**
 * A second visualization trait is refused, and refused by naming both.
 *
 * A trait of the same class REPLACES the one worked out for a class, which is the point of the feature. A
 * trait of a different class replaces nothing, so both build, and the parameters that decide how an entity
 * is drawn are a const shared fragment where the first value offered wins and the rest are dropped behind an
 * ensure. Allowing it produces a config that reads as configured and draws through the other trait.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityTraitsSecondVisualizationTest,
	"CrowdySDK.Mass.EntityTraitsRefuseASecondVisualizationTrait",
	CrowdyMassEntityTraitsDerivationTestFlags)
bool FCrowdyMassEntityTraitsSecondVisualizationTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Config(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeConfig());
	FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitToConfig<UMassVisualizationTrait>(
		Config.Get(), EProcessorExecutionFlags::AllNetModes);

	const TStrongObjectPtr<UCrowdyMassEntityTraitsTestVisualizationTrait> Rival(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeLooseTrait<UCrowdyMassEntityTraitsTestVisualizationTrait>(
			EProcessorExecutionFlags::AllNetModes));

	FString Refusal;
	FString Overridden;
	TestFalse(TEXT("a visualization trait of another class is not added beside the one already there"),
		FCrowdyMassEntityTraitsDerivation::CanAppend(*Config, *Rival, Refusal, Overridden));
	TestTrue(TEXT("and the refusal names the trait that was listed"),
		Refusal.Contains(TEXT("CrowdyMassEntityTraitsTestVisualizationTrait")));
	TestTrue(TEXT("as well as the one already deciding how the entity is drawn"),
		Refusal.Contains(TEXT("MassVisualizationTrait")));
	TestTrue(TEXT("a refusal is not an override, so nothing is reported as replaced"), Overridden.IsEmpty());

	const TStrongObjectPtr<UMassVisualizationTrait> SameClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeLooseTrait<UMassVisualizationTrait>(
			EProcessorExecutionFlags::AllNetModes));

	TestTrue(TEXT("but a visualization trait of the SAME class replaces it and is allowed"),
		FCrowdyMassEntityTraitsDerivation::CanAppend(*Config, *SameClass, Refusal, Overridden));

	return true;
}

/**
 * A trait that replaces one worked out for the class says which one it replaced.
 *
 * The replacement itself is Mass's doing rather than this code's: it keeps the last trait of a class it is
 * offered. What is missing without this is any way to find out, because the engine's own line about it names
 * the trait OBJECT and neither the class it replaced nor the actor class that asked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityTraitsOverrideReportTest,
	"CrowdySDK.Mass.EntityTraitsReportWhatADeclaredTraitReplaces",
	CrowdyMassEntityTraitsDerivationTestFlags)
bool FCrowdyMassEntityTraitsOverrideReportTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Config(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeConfig());
	FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitToConfig<UCrowdyMassEntityTraitsTestTrait>(
		Config.Get(), EProcessorExecutionFlags::AllNetModes);

	const TStrongObjectPtr<UCrowdyMassEntityTraitsTestTrait> SameClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeLooseTrait<UCrowdyMassEntityTraitsTestTrait>(
			EProcessorExecutionFlags::AllNetModes));

	FString Refusal;
	FString Overridden;
	TestTrue(TEXT("a trait of a class already there is allowed, because replacing it is the point"),
		FCrowdyMassEntityTraitsDerivation::CanAppend(*Config, *SameClass, Refusal, Overridden));
	TestEqual(TEXT("and it says which class it replaced"), Overridden,
		FString(TEXT("CrowdyMassEntityTraitsTestTrait")));

	const TStrongObjectPtr<UCrowdyMassEntityTraitsTestSecondTrait> OtherClass(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeLooseTrait<UCrowdyMassEntityTraitsTestSecondTrait>(
			EProcessorExecutionFlags::AllNetModes));

	TestTrue(TEXT("a trait of a class nothing there carries is allowed too"),
		FCrowdyMassEntityTraitsDerivation::CanAppend(*Config, *OtherClass, Refusal, Overridden));
	TestTrue(TEXT("and replaces nothing, so it says nothing"), Overridden.IsEmpty());

	return true;
}

/**
 * Two traits of one class that never build in the same net mode are not in each other's way.
 *
 * Mass keeps both on purpose, which is how a project gives a client and a server different configurations of
 * the same trait. Reporting one as replacing the other would send an author looking for a conflict that does
 * not exist, and refusing it would break the authoring Mass itself supports.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassEntityTraitsNetModeTest,
	"CrowdySDK.Mass.EntityTraitsAllowTheSameTraitForADifferentNetMode",
	CrowdyMassEntityTraitsDerivationTestFlags)
bool FCrowdyMassEntityTraitsNetModeTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UMassEntityConfigAsset> Config(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeConfig());
	FCrowdyMassEntityTraitsDerivationTestFixture::AddTraitToConfig<UCrowdyMassEntityTraitsTestTrait>(
		Config.Get(), EProcessorExecutionFlags::Server);

	const TStrongObjectPtr<UCrowdyMassEntityTraitsTestTrait> ClientOnly(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeLooseTrait<UCrowdyMassEntityTraitsTestTrait>(
			EProcessorExecutionFlags::Client));

	FString Refusal;
	FString Overridden;
	TestTrue(TEXT("a trait for another net mode is added"),
		FCrowdyMassEntityTraitsDerivation::CanAppend(*Config, *ClientOnly, Refusal, Overridden));
	TestTrue(TEXT("and replaces nothing, because the two never build together"), Overridden.IsEmpty());

	const TStrongObjectPtr<UCrowdyMassEntityTraitsTestTrait> ServerToo(
		FCrowdyMassEntityTraitsDerivationTestFixture::MakeLooseTrait<UCrowdyMassEntityTraitsTestTrait>(
			EProcessorExecutionFlags::Server));

	TestTrue(TEXT("while one for the same net mode is added as well"),
		FCrowdyMassEntityTraitsDerivation::CanAppend(*Config, *ServerToo, Refusal, Overridden));
	TestFalse(TEXT("and that one IS reported as replacing what was there"), Overridden.IsEmpty());

	return true;
}

#endif
