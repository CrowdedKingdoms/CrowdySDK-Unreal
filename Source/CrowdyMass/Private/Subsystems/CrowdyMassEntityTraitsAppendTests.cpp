#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimSequenceTransformProviderData.h"
#include "Assets/CrowdyMassAnimSet.h"
#include "Components/ActorComponent.h"
#include "Components/CrowdyMassAnimSetComponent.h"
#include "Components/CrowdyMassEntityTraitsComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Derivation/CrowdyMassEntityTraitsTestTypes.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityTraitBase.h"
#include "MassSkinnedMeshRepresentationTypes.h"
#include "MassVisualizationTrait.h"
#include "Misc/AutomationTest.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Traits/CrowdyMassAnimationTrait.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// What the traits a class declares actually DO once a config is built from that class. The derivation cases
// prove the reading; these prove the config that comes out of it, which is where the two decisions that
// cannot be read off the declaration live: where the declared traits sit relative to the ones worked out for
// the class, and whose objects the config ends up holding.
//
// Nothing here needs a world. A config built from a class is worked out from the class alone, so the
// subsystem is constructed directly and asked for one; only spawning from it would need a world.
static constexpr EAutomationTestFlags CrowdyMassEntityTraitsAppendTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

struct FCrowdyMassEntityTraitsAppendTestFixture
{
	// A subsystem with no world and no Initialize, which is all these cases need: building a config from a
	// class reads authored data and loads assets, and touches neither the entity manager nor the router.
	static UCrowdyMassEntitySubsystem* MakeSubsystem()
	{
		UCrowdyMassEntitySubsystem* Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());

		// Installs the suppression with no binding, so a class nothing else configures resolves to nothing
		// here rather than to whatever the running project happens to ship.
		Subsystem->SetAuthoredEntityConfigForTest(nullptr, nullptr);
		return Subsystem;
	}

	/**
	 * Stands in for a COMPILED Blueprint class, which is the only kind derivation answers for: it carries the
	 * parent's constructor, is linked against that parent so it has the parent's size, and then has a default
	 * object built. Building a default object before the link allocates a zero-sized object and runs an
	 * actor's constructor over it, which corrupts the heap silently.
	 */
	static UBlueprintGeneratedClass* MakeCompiledActorClass()
	{
		UClass* ParentClass = AActor::StaticClass();

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

	// Added as a root node, or under Parent when one is given.
	static USCS_Node* AddComponentNode(UBlueprintGeneratedClass* GeneratedClass, UActorComponent* Template, USCS_Node* Parent = nullptr)
	{
		if (!GeneratedClass->SimpleConstructionScript)
		{
			GeneratedClass->SimpleConstructionScript =
				NewObject<USimpleConstructionScript>(GeneratedClass, NAME_None, RF_Transient);
		}

		USCS_Node* Node = NewObject<USCS_Node>(GeneratedClass->SimpleConstructionScript, NAME_None, RF_Transient);
		Node->ComponentClass = Template->GetClass();
		Node->ComponentTemplate = Template;
		if (Parent)
		{
			Parent->AddChildNode(Node);
		}
		else
		{
			GeneratedClass->SimpleConstructionScript->AddNode(Node);
		}
		return Node;
	}

	// A config is only built for a class that supplies something to draw, so every class here carries a mesh.
	// An empty asset is enough: nothing compares anything but the pointer.
	static USkeletalMeshComponent* AddMesh(UBlueprintGeneratedClass* GeneratedClass, USCS_Node* Parent = nullptr)
	{
		USkeletalMeshComponent* Template =
			NewObject<USkeletalMeshComponent>(GeneratedClass, NAME_None, RF_Transient);
		Template->SetSkinnedAssetAndUpdate(
			NewObject<USkeletalMesh>(GetTransientPackage(), NAME_None, RF_Transient));

		AddComponentNode(GeneratedClass, Template, Parent);
		return Template;
	}

	static UCrowdyMassEntityTraitsComponent* AddTraitsComponent(UBlueprintGeneratedClass* GeneratedClass,
		const int32 Marker)
	{
		UCrowdyMassEntityTraitsComponent* Template =
			NewObject<UCrowdyMassEntityTraitsComponent>(GeneratedClass, NAME_None, RF_Transient);

		UCrowdyMassEntityTraitsTestTrait* Trait =
			NewObject<UCrowdyMassEntityTraitsTestTrait>(Template, NAME_None, RF_Transient);
		Trait->Marker = Marker;
		Template->Traits.Add(Trait);

		AddComponentNode(GeneratedClass, Template);
		return Template;
	}

	// A class that supplies a mesh and lists one trait of its own, which is the whole authoring story this
	// feature exists for.
	static UBlueprintGeneratedClass* MakeDeclaringClass(const int32 Marker,
		UCrowdyMassEntityTraitsComponent** OutComponent = nullptr)
	{
		UBlueprintGeneratedClass* GeneratedClass = MakeCompiledActorClass();
		AddMesh(GeneratedClass);

		UCrowdyMassEntityTraitsComponent* Component = AddTraitsComponent(GeneratedClass, Marker);
		if (OutComponent)
		{
			*OutComponent = Component;
		}

		return GeneratedClass;
	}

	static const UCrowdyMassEntityTraitsTestTrait* FindDeclaredTrait(const UMassEntityConfigAsset& Config)
	{
		for (const UMassEntityTraitBase* Trait : Config.GetConfig().GetTraits())
		{
			if (const UCrowdyMassEntityTraitsTestTrait* Found = Cast<UCrowdyMassEntityTraitsTestTrait>(Trait))
			{
				return Found;
			}
		}

		return nullptr;
	}

	static UAnimSequenceTransformProviderData* MakeBank()
	{
		return NewObject<UAnimSequenceTransformProviderData>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	// Gives the class an anim set component naming a new set, whose Bank is BankOrNull. Added beside the
	// mesh node rather than replacing the construction script, which would discard the mesh.
	static UCrowdyMassAnimSet* AddAnimSet(UBlueprintGeneratedClass* GeneratedClass,
		UAnimSequenceTransformProviderData* BankOrNull)
	{
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);
		AnimSet->Bank = TSoftObjectPtr<UAnimSequenceTransformProviderData>(BankOrNull);

		UCrowdyMassAnimSetComponent* Template =
			NewObject<UCrowdyMassAnimSetComponent>(GeneratedClass, NAME_None, RF_Transient);
		Template->AnimSet = TSoftObjectPtr<UCrowdyMassAnimSet>(AnimSet);

		AddComponentNode(GeneratedClass, Template);
		return AnimSet;
	}

	static const UMassVisualizationTrait* FindVisualizationTrait(const UMassEntityConfigAsset& Config)
	{
		for (const UMassEntityTraitBase* Trait : Config.GetConfig().GetTraits())
		{
			if (const UMassVisualizationTrait* Found = Cast<UMassVisualizationTrait>(Trait))
			{
				return Found;
			}
		}

		return nullptr;
	}

	// The provider the first mesh of the config's visualization trait carries, or null when there is no such
	// trait or mesh.
	static const UObject* FindMeshProvider(const UMassEntityConfigAsset& Config)
	{
		const UMassVisualizationTrait* Visualization = FindVisualizationTrait(Config);
		if (!Visualization || Visualization->SkinnedMeshInstanceDesc.Meshes.IsEmpty())
		{
			return nullptr;
		}

		return Visualization->SkinnedMeshInstanceDesc.Meshes[0].TransformProvider.Get();
	}
};

/**
 * A class that lists a trait of its own gets it on the config built from that class.
 *
 * This is the whole point of the feature: before it, one fragment of a game's own cost that game authoring
 * the entire Mass entity config by hand and giving up the mesh, collision, level table and animation the
 * class supplies for free.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDerivedConfigCarriesDeclaredTraitsTest,
	"CrowdySDK.Mass.DerivedConfigCarriesTheTraitsAClassDeclares", CrowdyMassEntityTraitsAppendTestFlags)
bool FCrowdyMassDerivedConfigCarriesDeclaredTraitsTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeSubsystem());
	const TStrongObjectPtr<UBlueprintGeneratedClass> DeclaringClass(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeDeclaringClass(7));

	const UMassEntityConfigAsset* Config = Subsystem->GetOrBuildDerivedEntityConfigForTest(DeclaringClass.Get());
	if (!TestNotNull(TEXT("a class that supplies a mesh gets a config built from it"), Config))
	{
		return false;
	}

	const UCrowdyMassEntityTraitsTestTrait* Declared =
		FCrowdyMassEntityTraitsAppendTestFixture::FindDeclaredTrait(*Config);
	if (!TestNotNull(TEXT("and that config carries the trait the class listed"), Declared))
	{
		return false;
	}

	TestEqual(TEXT("carrying the values authored on it rather than a fresh trait of the same class"),
		Declared->Marker, 7);

	const TStrongObjectPtr<UBlueprintGeneratedClass> BareClass(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeCompiledActorClass());
	FCrowdyMassEntityTraitsAppendTestFixture::AddMesh(BareClass.Get());

	const UMassEntityConfigAsset* BareConfig = Subsystem->GetOrBuildDerivedEntityConfigForTest(BareClass.Get());
	if (!TestNotNull(TEXT("a class that lists nothing still gets a config"), BareConfig))
	{
		return false;
	}

	TestNull(TEXT("and that config carries no trait of a game's own, so nothing arrives uninvited"),
		FCrowdyMassEntityTraitsAppendTestFixture::FindDeclaredTrait(*BareConfig));

	return true;
}

/**
 * The config holds a COPY of the declared trait, owned by the config, not the instance the class owns.
 *
 * The instance in the list belongs to a component template on the class, and recompiling that class replaces
 * every object the template owns. A config built to be kept for as long as the class is loaded cannot hold
 * one of those. The class and value assertions in the case above stay green under a shared pointer, which is
 * exactly why this is a separate case: "a trait arrived" says nothing about whose it is.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDeclaredTraitsAreCopiedTest,
	"CrowdySDK.Mass.DeclaredTraitsAreCopiedRatherThanShared", CrowdyMassEntityTraitsAppendTestFlags)
bool FCrowdyMassDeclaredTraitsAreCopiedTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeSubsystem());

	UCrowdyMassEntityTraitsComponent* Component = nullptr;
	const TStrongObjectPtr<UBlueprintGeneratedClass> DeclaringClass(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeDeclaringClass(11, &Component));

	if (!TestNotNull(TEXT("the class carries the component the trait was listed on"), Component))
	{
		return false;
	}

	const UMassEntityConfigAsset* Config = Subsystem->GetOrBuildDerivedEntityConfigForTest(DeclaringClass.Get());
	if (!TestNotNull(TEXT("and a config was built from it"), Config))
	{
		return false;
	}

	const UCrowdyMassEntityTraitsTestTrait* InConfig =
		FCrowdyMassEntityTraitsAppendTestFixture::FindDeclaredTrait(*Config);
	if (!TestNotNull(TEXT("which carries the trait"), InConfig))
	{
		return false;
	}

	TestNotSamePtr(TEXT("the config holds a copy, not the instance the class's component owns"),
		static_cast<const UObject*>(InConfig), static_cast<const UObject*>(Component->Traits[0].Get()));
	TestSamePtr(TEXT("and that copy belongs to the config, so its lifetime is the config's"),
		static_cast<const UObject*>(InConfig->GetOuter()), static_cast<const UObject*>(Config));
	TestSamePtr(TEXT("the copy keeps the source's concrete class, which is what makes Mass treat it as a replacement"),
		static_cast<const UStruct*>(InConfig->GetClass()),
		static_cast<const UStruct*>(UCrowdyMassEntityTraitsTestTrait::StaticClass()));
	TestEqual(TEXT("and the values authored on the source"), InConfig->Marker, 11);

	return true;
}

/**
 * A declared trait is added after every trait worked out for the class.
 *
 * Mass combines a config's traits by class and keeps the LAST one of each class it is offered, so position
 * is the whole of the override rule. Anywhere earlier and a game's own trait would be listed in the config
 * and then thrown away by the one worked out for it, which is authored, present, and never built.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDeclaredTraitsComeLastTest,
	"CrowdySDK.Mass.DeclaredTraitsAreAddedAfterTheDerivedOnes", CrowdyMassEntityTraitsAppendTestFlags)
bool FCrowdyMassDeclaredTraitsComeLastTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeSubsystem());
	const TStrongObjectPtr<UBlueprintGeneratedClass> DeclaringClass(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeDeclaringClass(3));

	const UMassEntityConfigAsset* Config = Subsystem->GetOrBuildDerivedEntityConfigForTest(DeclaringClass.Get());
	if (!TestNotNull(TEXT("a config was built from the class"), Config))
	{
		return false;
	}

	const TConstArrayView<UMassEntityTraitBase*> Traits = Config->GetConfig().GetTraits();
	if (!TestTrue(TEXT("which carries traits worked out for the class as well as the declared one"),
		Traits.Num() > 1))
	{
		return false;
	}

	TestNotNull(TEXT("and the LAST of them is the trait the class declared"),
		Cast<UCrowdyMassEntityTraitsTestTrait>(Traits[Traits.Num() - 1]));

	return true;
}

/**
 * A class an authored config answers for is told once that its declared traits are not read, and only when
 * it declares some.
 *
 * A config's trait list is the whole statement of what its entities are made of, so this feature deliberately
 * does not add to one. That decision has no symptom: the crowd appears and behaves correctly, and the only
 * thing wrong is that a list somebody filled in does nothing. Saying it once per class rather than once per
 * entity is what keeps the one line naming the fix findable.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAuthoredConfigShadowsDeclaredTraitsTest,
	"CrowdySDK.Mass.AnAuthoredConfigIsNotGivenTheTraitsAClassDeclares", CrowdyMassEntityTraitsAppendTestFlags)
bool FCrowdyMassAuthoredConfigShadowsDeclaredTraitsTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeSubsystem());
	const TStrongObjectPtr<UMassEntityConfigAsset> Authored(
		NewObject<UMassEntityConfigAsset>(GetTransientPackage(), NAME_None, RF_Transient));
	const TStrongObjectPtr<UBlueprintGeneratedClass> DeclaringClass(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeDeclaringClass(5));

	TestTrue(TEXT("a class whose declared traits an authored config shadows is told so"),
		Subsystem->ReportDeclaredTraitsNotConsumedForTest(DeclaringClass.Get(), Authored.Get()));
	TestFalse(TEXT("and told once, not once per entity of it that appears"),
		Subsystem->ReportDeclaredTraitsNotConsumedForTest(DeclaringClass.Get(), Authored.Get()));

	TestEqual(TEXT("and the authored config is left exactly as it was authored"),
		Authored->GetConfig().GetTraits().Num(), 0);

	const TStrongObjectPtr<UBlueprintGeneratedClass> BareClass(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeCompiledActorClass());
	FCrowdyMassEntityTraitsAppendTestFixture::AddMesh(BareClass.Get());

	TestFalse(TEXT("while a class that declares nothing has nothing shadowed and says nothing"),
		Subsystem->ReportDeclaredTraitsNotConsumedForTest(BareClass.Get(), Authored.Get()));

	return true;
}

/**
 * A config built from a class draws through the Bank its anim set names.
 *
 * Before this the set's Bank was editor-only, so a derived class had an animation trait and a mesh with no
 * transform provider and stood in its bind pose. Pointer identity is the whole assertion: the bank is the
 * asset the author named, carried on the mesh the instances are drawn from.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDerivedConfigCarriesAnimSetBankTest,
	"CrowdySDK.Mass.DerivedConfigMeshCarriesTheAnimSetBank", CrowdyMassEntityTraitsAppendTestFlags)
bool FCrowdyMassDerivedConfigCarriesAnimSetBankTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassEntityTraitsAppendTestFixture;

	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(Fixture::MakeSubsystem());
	const TStrongObjectPtr<UAnimSequenceTransformProviderData> Bank(Fixture::MakeBank());
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(Fixture::MakeCompiledActorClass());
	Fixture::AddMesh(Class.Get());
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::AddAnimSet(Class.Get(), Bank.Get()));

	const UMassEntityConfigAsset* Config = Subsystem->GetOrBuildDerivedEntityConfigForTest(Class.Get());
	if (!TestNotNull(TEXT("a class with a mesh and an anim set gets a config built from it"), Config))
	{
		return false;
	}

	const UMassVisualizationTrait* Visualization = Fixture::FindVisualizationTrait(*Config);
	if (!TestNotNull(TEXT("which carries a visualization trait"), Visualization))
	{
		return false;
	}

	if (!TestEqual(TEXT("with the one mesh found on the class"), Visualization->SkinnedMeshInstanceDesc.Meshes.Num(), 1))
	{
		return false;
	}

	TestSamePtr(TEXT("whose transform provider is the Bank the anim set names"),
		Fixture::FindMeshProvider(*Config), static_cast<const UObject*>(Bank.Get()));

	return true;
}

/**
 * A config built from a class draws its mesh where the class places it.
 *
 * The crowd draws at the mesh entry's LocalTransform on top of the entity's transform, and a character sends
 * its capsule centre, so a derived entry left at identity floats a row by the capsule's half height and turns
 * it a quarter away from the actor it stands for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDerivedConfigPlacesTheMeshTest,
	"CrowdySDK.Mass.DerivedConfigMeshIsPlacedWhereTheClassPlacesIt", CrowdyMassEntityTraitsAppendTestFlags)
bool FCrowdyMassDerivedConfigPlacesTheMeshTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassEntityTraitsAppendTestFixture;

	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(Fixture::MakeSubsystem());
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(Fixture::MakeCompiledActorClass());
	const FTransform Placement(FRotator(0.0, -90.0, 0.0), FVector(0.0, 0.0, -90.0));
	USCS_Node* Root = Fixture::AddComponentNode(Class.Get(), NewObject<USceneComponent>(Class.Get(), NAME_None, RF_Transient));
	Fixture::AddMesh(Class.Get(), Root)->SetRelativeTransform(Placement);

	const UMassEntityConfigAsset* Config = Subsystem->GetOrBuildDerivedEntityConfigForTest(Class.Get());
	if (!TestNotNull(TEXT("a class with a mesh gets a config built from it"), Config))
	{
		return false;
	}

	const UMassVisualizationTrait* Visualization = Fixture::FindVisualizationTrait(*Config);
	if (!TestNotNull(TEXT("which carries a visualization trait"), Visualization)
		|| !TestEqual(TEXT("with the one mesh found on the class"), Visualization->SkinnedMeshInstanceDesc.Meshes.Num(), 1))
	{
		return false;
	}

	TestTrue(TEXT("whose local transform is the placement the class gives its mesh"),
		Visualization->SkinnedMeshInstanceDesc.Meshes[0].LocalTransform.Equals(Placement));

	return true;
}

/**
 * A set that names no Bank leaves the mesh without a transform provider.
 *
 * The config is still built and the animation trait still names the set: the class appears and moves, in
 * its bind pose, and says so once. A bank invented here would draw some other class's clips.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDerivedConfigWithoutBankHasNoProviderTest,
	"CrowdySDK.Mass.DerivedConfigMeshHasNoProviderWhenTheAnimSetNamesNoBank", CrowdyMassEntityTraitsAppendTestFlags)
bool FCrowdyMassDerivedConfigWithoutBankHasNoProviderTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassEntityTraitsAppendTestFixture;

	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(Fixture::MakeSubsystem());
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(Fixture::MakeCompiledActorClass());
	Fixture::AddMesh(Class.Get());
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::AddAnimSet(Class.Get(), nullptr));

	const UMassEntityConfigAsset* Config = Subsystem->GetOrBuildDerivedEntityConfigForTest(Class.Get());
	if (!TestNotNull(TEXT("a set with no Bank still gets a config built from the class"), Config))
	{
		return false;
	}

	if (!TestNotNull(TEXT("which carries a visualization trait"), Fixture::FindVisualizationTrait(*Config)))
	{
		return false;
	}

	TestNull(TEXT("whose mesh has no transform provider"), Fixture::FindMeshProvider(*Config));

	return true;
}

/**
 * A class with no anim set component has no Bank to give its mesh.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDerivedConfigWithoutAnimSetHasNoProviderTest,
	"CrowdySDK.Mass.DerivedConfigMeshHasNoProviderWhenTheClassHasNoAnimSet", CrowdyMassEntityTraitsAppendTestFlags)
bool FCrowdyMassDerivedConfigWithoutAnimSetHasNoProviderTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassEntityTraitsAppendTestFixture;

	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(Fixture::MakeSubsystem());
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(Fixture::MakeCompiledActorClass());
	Fixture::AddMesh(Class.Get());

	const UMassEntityConfigAsset* Config = Subsystem->GetOrBuildDerivedEntityConfigForTest(Class.Get());
	if (!TestNotNull(TEXT("a class with only a mesh gets a config built from it"), Config))
	{
		return false;
	}

	if (!TestNotNull(TEXT("which carries a visualization trait"), Fixture::FindVisualizationTrait(*Config)))
	{
		return false;
	}

	TestNull(TEXT("whose mesh has no transform provider"), Fixture::FindMeshProvider(*Config));

	return true;
}

#if WITH_EDITOR

/**
 * Editing an anim set's Bank drops the derived config built from it.
 *
 * The config copies the Bank into its mesh description, so without the drop an edit would change nothing
 * for the rest of the editor session and the old bank would keep drawing with no error.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetBankEditDropsDerivedConfigTest,
	"CrowdySDK.Mass.EditingAnAnimSetBankDropsTheDerivedConfig", CrowdyMassEntityTraitsAppendTestFlags)
bool FCrowdyMassAnimSetBankEditDropsDerivedConfigTest::RunTest(const FString& Parameters)
{
	using Fixture = FCrowdyMassEntityTraitsAppendTestFixture;

	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(Fixture::MakeSubsystem());
	const TStrongObjectPtr<UAnimSequenceTransformProviderData> OldBank(Fixture::MakeBank());
	const TStrongObjectPtr<UAnimSequenceTransformProviderData> NewBank(Fixture::MakeBank());
	const TStrongObjectPtr<UBlueprintGeneratedClass> Class(Fixture::MakeCompiledActorClass());
	Fixture::AddMesh(Class.Get());
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(Fixture::AddAnimSet(Class.Get(), OldBank.Get()));

	const UMassEntityConfigAsset* First = Subsystem->GetOrBuildDerivedEntityConfigForTest(Class.Get());
	if (!TestNotNull(TEXT("a config was built from the class"), First))
	{
		return false;
	}

	AnimSet->Bank = TSoftObjectPtr<UAnimSequenceTransformProviderData>(NewBank.Get());

	TestSamePtr(TEXT("the config is kept until something reports the edit"),
		static_cast<const UObject*>(Subsystem->GetOrBuildDerivedEntityConfigForTest(Class.Get())),
		static_cast<const UObject*>(First));

	Subsystem->HandleObjectPropertyChangedForTest(AnimSet.Get());

	const UMassEntityConfigAsset* Rebuilt = Subsystem->GetOrBuildDerivedEntityConfigForTest(Class.Get());
	if (!TestNotNull(TEXT("and a config is built again afterwards"), Rebuilt))
	{
		return false;
	}

	TestNotSamePtr(TEXT("which is a new one, not the config holding the old bank"),
		static_cast<const UObject*>(Rebuilt), static_cast<const UObject*>(First));
	TestSamePtr(TEXT("carrying the Bank the set names now"),
		Fixture::FindMeshProvider(*Rebuilt), static_cast<const UObject*>(NewBank.Get()));

	return true;
}

/**
 * Listing or editing a trait in the details panel drops the config already built from that class.
 *
 * A config built from a class holds a COPY of what the class listed, so nothing about editing the original
 * reaches it: without this the config would keep saying what the class used to say for the rest of the
 * editor session, and adding the trait would appear to do nothing at all.
 *
 * Both halves are asserted because they arrive as two different objects. Adding or removing a row edits the
 * component's own array and reports the COMPONENT; changing a field inside a listed trait reports the TRAIT.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassDeclaredTraitEditDropsDerivedConfigTest,
	"CrowdySDK.Mass.EditingADeclaredTraitDropsTheDerivedConfig", CrowdyMassEntityTraitsAppendTestFlags)
bool FCrowdyMassDeclaredTraitEditDropsDerivedConfigTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeSubsystem());

	UCrowdyMassEntityTraitsComponent* Component = nullptr;
	const TStrongObjectPtr<UBlueprintGeneratedClass> DeclaringClass(
		FCrowdyMassEntityTraitsAppendTestFixture::MakeDeclaringClass(2, &Component));

	const UMassEntityConfigAsset* First = Subsystem->GetOrBuildDerivedEntityConfigForTest(DeclaringClass.Get());
	if (!TestNotNull(TEXT("a config was built from the class"), First))
	{
		return false;
	}

	TestSamePtr(TEXT("and is kept, so a class is not walked again for every entity of it"),
		static_cast<const UObject*>(Subsystem->GetOrBuildDerivedEntityConfigForTest(DeclaringClass.Get())),
		static_cast<const UObject*>(First));

	Subsystem->HandleObjectPropertyChangedForTest(Component);

	const UMassEntityConfigAsset* AfterComponentEdit =
		Subsystem->GetOrBuildDerivedEntityConfigForTest(DeclaringClass.Get());
	TestNotSamePtr(TEXT("adding or removing a row on the component drops what was built from it"),
		static_cast<const UObject*>(AfterComponentEdit), static_cast<const UObject*>(First));

	Subsystem->HandleObjectPropertyChangedForTest(Component->Traits[0].Get());

	TestNotSamePtr(TEXT("and so does editing a field inside a listed trait, which reports a different object"),
		static_cast<const UObject*>(Subsystem->GetOrBuildDerivedEntityConfigForTest(DeclaringClass.Get())),
		static_cast<const UObject*>(AfterComponentEdit));

	return true;
}

#endif

#endif
