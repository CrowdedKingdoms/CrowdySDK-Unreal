#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimSequence.h"
#include "Assets/CrowdyMassAnimSet.h"
#include "Components/CrowdyMassAnimSetComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Mass/CrowdyMassActionLibrary.h"
#include "Mass/CrowdyMassActionLibraryTestTypes.h"
#include "MassEntityConfigAsset.h"
#include "Misc/AutomationTest.h"
#include "Subsystems/CrowdyMassEntitySubsystem.h"
#include "Traits/CrowdyMassAnimationTrait.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

// An animation set is per CLASS, so every question here is asked about a class, and the interesting cases
// are the ones where two classes must not answer alike and where an absent answer must not collapse into
// one answer.
//
// Nothing here needs a world. The subsystem is constructed directly and its authored-config route is
// installed through a test seam, which also stops the running project's own Remote Entity Config from
// answering for a class these cases mean to leave unconfigured.
//
// Enum results are compared as bytes throughout, matching the other subsystem cases: the automation
// framework's generic comparison wants a way to print the value, which a plain enum does not have.
static constexpr EAutomationTestFlags CrowdyMassAnimSetResolutionTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

struct FCrowdyMassAnimSetResolutionTestFixture
{
	// A subsystem with no world and no Initialize, which is all these cases need: resolution reads
	// authored data and loads assets, and touches neither the entity manager nor the router.
	static UCrowdyMassEntitySubsystem* MakeSubsystem()
	{
		UCrowdyMassEntitySubsystem* Subsystem = NewObject<UCrowdyMassEntitySubsystem>(GetTransientPackage());

		// Installs the suppression with no binding, so a class nothing else configures resolves to nothing
		// here rather than to whatever the running project happens to ship.
		Subsystem->SetAuthoredEntityConfigForTest(nullptr, nullptr);
		return Subsystem;
	}

	// A set whose rows are named and whose sequences are distinct transient assets, which is enough:
	// nothing here resolves a row against a bank, it only turns names and ids into rows.
	static UCrowdyMassAnimSet* MakeAnimSet(const TArray<FName>& PoseNames, const int32 ActionCount = 0)
	{
		UCrowdyMassAnimSet* AnimSet = NewObject<UCrowdyMassAnimSet>(GetTransientPackage(), NAME_None, RF_Transient);

		for (const FName& PoseName : PoseNames)
		{
			FCrowdyMassPoseEntry Pose;
			Pose.PoseName = PoseName;
			Pose.Sequence = TSoftObjectPtr<UAnimSequence>(MakeSequence());
			AnimSet->Poses.Add(Pose);
		}

		for (int32 ActionIndex = 0; ActionIndex < ActionCount; ++ActionIndex)
		{
			FCrowdyMassActionEntry Action;
			Action.ActionName = FName(*FString::Printf(TEXT("Action%d"), ActionIndex));
			Action.Sequence = TSoftObjectPtr<UAnimSequence>(MakeSequence());
			AnimSet->Actions.Add(Action);
		}

		return AnimSet;
	}

	static UAnimSequence* MakeSequence()
	{
		return NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	static TSoftObjectPtr<UCrowdyMassAnimSet> NoAnimSet()
	{
		return TSoftObjectPtr<UCrowdyMassAnimSet>();
	}

	/**
	 * A Mass entity config, optionally carrying an animation trait, optionally naming a set on it.
	 *
	 * The three shapes are the whole precedence question: a config with no trait, a config with a trait
	 * that names nothing, and a config with a trait that names a set. Only the last of them answers.
	 */
	static UMassEntityConfigAsset* MakeConfig(const bool bWithAnimationTrait,
		const TSoftObjectPtr<UCrowdyMassAnimSet>& AnimSet)
	{
		UMassEntityConfigAsset* Config = NewObject<UMassEntityConfigAsset>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!bWithAnimationTrait)
		{
			return Config;
		}

		UCrowdyMassAnimationTrait* Trait = NewObject<UCrowdyMassAnimationTrait>(Config, NAME_None, RF_Transient);
		Trait->AnimSet = AnimSet;
		Config->GetMutableConfig().AddTrait(*Trait);
		return Config;
	}

	/**
	 * Stands in for a COMPILED Blueprint class, which is the only kind class derivation answers for. Three
	 * things make it one rather than a half-formed class: it carries the parent's constructor, without
	 * which a default object cannot be built at all; it is linked against that parent, which is what gives
	 * it the parent's size; and it then has that default object built.
	 *
	 * The link is not optional and its absence is silent. A class assembled with NewObject has a properties
	 * size of zero, and SetSuperStruct does not change that: only linking copies the parent's size across.
	 * Building a default object before that allocates a zero-sized object and then runs the parent's
	 * constructor over it, which writes an actor's worth of members past the end of the allocation.
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

	// The shape a component added in the Blueprint editor takes: a construction script node holding a
	// component template, with no matching component anywhere on a default object. That is the route a
	// default-object read alone would miss, and it is the one an author actually uses.
	static UCrowdyMassAnimSetComponent* AddAnimSetComponent(UBlueprintGeneratedClass* GeneratedClass,
		const TSoftObjectPtr<UCrowdyMassAnimSet>& AnimSet)
	{
		USimpleConstructionScript* Script =
			NewObject<USimpleConstructionScript>(GeneratedClass, NAME_None, RF_Transient);
		GeneratedClass->SimpleConstructionScript = Script;

		UCrowdyMassAnimSetComponent* Template =
			NewObject<UCrowdyMassAnimSetComponent>(GeneratedClass, NAME_None, RF_Transient);
		Template->AnimSet = AnimSet;

		USCS_Node* Node = NewObject<USCS_Node>(Script, NAME_None, RF_Transient);
		Node->ComponentClass = UCrowdyMassAnimSetComponent::StaticClass();
		Node->ComponentTemplate = Template;
		Script->AddNode(Node);

		return Template;
	}

	/**
	 * A reference that names something real and yields no animation set: a path to an object of another
	 * type, which resolves instantly and casts to nothing.
	 *
	 * Deliberately not a path to a missing package. That would exercise the same branch while filling the
	 * log with loader complaints whose exact wording a case would then have to whitelist, and what is under
	 * test is what the subsystem does with a reference that hands back no set.
	 */
	static TSoftObjectPtr<UCrowdyMassAnimSet> MakeUnloadableReference()
	{
		return TSoftObjectPtr<UCrowdyMassAnimSet>(FSoftObjectPath(AActor::StaticClass()));
	}
};

// The shape this whole surface is most likely to get wrong: a class that names no set, a class whose set
// will not load, and a class nobody has asked about are three different faults with three different fixes,
// and all three hand back a null pointer. One branch for all of them turns "nobody has looked" into "there
// is nothing there".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetThreeMissesTest,
	"CrowdySDK.Mass.AnimSetResolutionTellsItsThreeMissesApart", CrowdyMassAnimSetResolutionTestFlags)
bool FCrowdyMassAnimSetThreeMissesTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UBlueprintGeneratedClass> SilentClass(
		FCrowdyMassAnimSetResolutionTestFixture::MakeCompiledActorClass());

	const TStrongObjectPtr<UBlueprintGeneratedClass> BrokenClass(
		FCrowdyMassAnimSetResolutionTestFixture::MakeCompiledActorClass());
	FCrowdyMassAnimSetResolutionTestFixture::AddAnimSetComponent(BrokenClass.Get(),
		FCrowdyMassAnimSetResolutionTestFixture::MakeUnloadableReference());

	// Asked before anything else, because asking is what stops this answer from being true.
	TestEqual(TEXT("a class nothing has asked about reports that nobody has asked"),
		static_cast<uint8>(Subsystem->GetResolvedAnimSetSource(SilentClass.Get())),
		static_cast<uint8>(ECrowdyMassAnimSetSource::NotResolved));

	ECrowdyMassAnimSetSource Source = ECrowdyMassAnimSetSource::ConfigTrait;

	TestNull(TEXT("no class resolves no set"), Subsystem->FindRemoteEntityAnimSet(nullptr, Source));
	TestEqual(TEXT("and says the class is what was missing, not the set"),
		static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::NoClass));

	TestNull(TEXT("a class that names no set anywhere resolves none"),
		Subsystem->FindRemoteEntityAnimSet(SilentClass.Get(), Source));
	TestEqual(TEXT("and says nothing declares one"),
		static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::Undeclared));

	TestNull(TEXT("a class whose named set will not load resolves none either"),
		Subsystem->FindRemoteEntityAnimSet(BrokenClass.Get(), Source));
	TestEqual(TEXT("but says the reference is broken rather than absent"),
		static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::LoadFailed));

	// The point of the peek: once a class HAS been asked about it stops reporting that nobody asked, so the
	// answer is about this class rather than about the map being empty.
	TestEqual(TEXT("an asked class no longer reports that nobody has asked"),
		static_cast<uint8>(Subsystem->GetResolvedAnimSetSource(SilentClass.Get())),
		static_cast<uint8>(ECrowdyMassAnimSetSource::Undeclared));
	TestEqual(TEXT("and a broken one keeps its own answer rather than its neighbour's"),
		static_cast<uint8>(Subsystem->GetResolvedAnimSetSource(BrokenClass.Get())),
		static_cast<uint8>(ECrowdyMassAnimSetSource::LoadFailed));

	return true;
}

// Precedence is a CAPABILITY question. A config that exists for collision or level-of-detail reasons and
// names no animation set must not answer "this class has no animation": the class's own declaration is what
// is asked next, and a config that cannot name a set makes that fallback unreachable if presence alone wins.
//
// It must also not overstate what falling through achieved. A spawn of a class an authored config is bound
// to is BUILT from that config, so a config with no animation trait installs no animation fragments and one
// with an empty trait installs a shared fragment holding nothing: either way the class's set is named and
// nothing renders it. That answer is ClassComponentNotRendered rather than ClassComponent, because every
// diagnostic that exists to explain a bind-posed crowd reads it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetFallThroughTest,
	"CrowdySDK.Mass.AnimSetResolutionFallsThroughAConfigThatNamesNoSet", CrowdyMassAnimSetResolutionTestFlags)
bool FCrowdyMassAnimSetFallThroughTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassAnimSet> ClassSet(
		FCrowdyMassAnimSetResolutionTestFixture::MakeAnimSet({TEXT("Dead")}));
	const TStrongObjectPtr<UCrowdyMassAnimSet> ConfigSet(
		FCrowdyMassAnimSetResolutionTestFixture::MakeAnimSet({TEXT("Dead")}));

	// One class shape, reused against all four config shapes, so the only thing differing between the
	// blocks below is what the config can say.
	const TStrongObjectPtr<UBlueprintGeneratedClass> DeclaringClass(
		FCrowdyMassAnimSetResolutionTestFixture::MakeCompiledActorClass());
	FCrowdyMassAnimSetResolutionTestFixture::AddAnimSetComponent(DeclaringClass.Get(),
		TSoftObjectPtr<UCrowdyMassAnimSet>(ClassSet.Get()));

	ECrowdyMassAnimSetSource Source = ECrowdyMassAnimSetSource::NoClass;

	{
		const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
			FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());
		const TStrongObjectPtr<UMassEntityConfigAsset> ConfigWithNoTrait(
			FCrowdyMassAnimSetResolutionTestFixture::MakeConfig(/*bWithAnimationTrait=*/false,
				FCrowdyMassAnimSetResolutionTestFixture::NoAnimSet()));
		Subsystem->SetAuthoredEntityConfigForTest(DeclaringClass.Get(), ConfigWithNoTrait.Get());

		TestSamePtr(TEXT("a config carrying no animation trait falls through to the class"),
			Subsystem->FindRemoteEntityAnimSet(DeclaringClass.Get(), Source),
			static_cast<const UCrowdyMassAnimSet*>(ClassSet.Get()));

		// The honest half of that answer. The spawn is still built from the authored config, which installs
		// no animation fragments at all, so the class's set is named and nothing renders it. Reporting a
		// plain success here is what would send someone looking at the anim set for a crowd that never
		// reaches the animation pass in the first place.
		TestEqual(TEXT("and says the class named it but nothing renders it"),
			static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::ClassComponentNotRendered));
	}

	{
		const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
			FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());
		const TStrongObjectPtr<UMassEntityConfigAsset> ConfigWithEmptyTrait(
			FCrowdyMassAnimSetResolutionTestFixture::MakeConfig(/*bWithAnimationTrait=*/true,
				FCrowdyMassAnimSetResolutionTestFixture::NoAnimSet()));
		Subsystem->SetAuthoredEntityConfigForTest(DeclaringClass.Get(), ConfigWithEmptyTrait.Get());

		TestSamePtr(TEXT("a config whose animation trait names nothing falls through too"),
			Subsystem->FindRemoteEntityAnimSet(DeclaringClass.Get(), Source),
			static_cast<const UCrowdyMassAnimSet*>(ClassSet.Get()));

		// Same answer for the same reason, though the shape differs: here the trait installs the animation
		// fragments and a shared fragment holding no set, so the entities reach the animation pass and
		// resolve nothing. Either way the set the class names is not the set anything renders.
		TestEqual(TEXT("and still says the class named it but nothing renders it"),
			static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::ClassComponentNotRendered));
	}

	{
		// The case the two above must not be confused with, and the one the whole distinction turns on:
		// with nothing authored for the class, the spawn IS built from what the class carries, so the
		// class's own set is what renders and the answer is a plain success.
		const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
			FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());

		TestSamePtr(TEXT("with no authored config at all the class's set is what renders"),
			Subsystem->FindRemoteEntityAnimSet(DeclaringClass.Get(), Source),
			static_cast<const UCrowdyMassAnimSet*>(ClassSet.Get()));
		TestEqual(TEXT("and it is reported as a plain success"),
			static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::ClassComponent));
	}

	{
		const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
			FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());
		const TStrongObjectPtr<UMassEntityConfigAsset> ConfigWithSet(
			FCrowdyMassAnimSetResolutionTestFixture::MakeConfig(/*bWithAnimationTrait=*/true,
				TSoftObjectPtr<UCrowdyMassAnimSet>(ConfigSet.Get())));
		Subsystem->SetAuthoredEntityConfigForTest(DeclaringClass.Get(), ConfigWithSet.Get());

		// The other half of the rule, and the half that stops the fall-through becoming a way to ignore an
		// author: a config that CAN name a set wins over the class's own declaration.
		TestSamePtr(TEXT("a config whose animation trait names a set wins over the class"),
			Subsystem->FindRemoteEntityAnimSet(DeclaringClass.Get(), Source),
			static_cast<const UCrowdyMassAnimSet*>(ConfigSet.Get()));
		TestEqual(TEXT("and says the config supplied it"),
			static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::ConfigTrait));
	}

	{
		// A config that names a set which will not load does NOT fall through. Falling through would render
		// this class from its own component's set while the config names a different one nobody can see is
		// missing, which is a wrong clip rather than an absent one.
		const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
			FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());
		const TStrongObjectPtr<UMassEntityConfigAsset> ConfigWithBrokenSet(
			FCrowdyMassAnimSetResolutionTestFixture::MakeConfig(/*bWithAnimationTrait=*/true,
				FCrowdyMassAnimSetResolutionTestFixture::MakeUnloadableReference()));
		Subsystem->SetAuthoredEntityConfigForTest(DeclaringClass.Get(), ConfigWithBrokenSet.Get());

		TestNull(TEXT("a config naming a set that will not load resolves nothing"),
			Subsystem->FindRemoteEntityAnimSet(DeclaringClass.Get(), Source));
		TestEqual(TEXT("and reports the broken reference rather than quietly using the class's set"),
			static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::LoadFailed));
	}

	return true;
}

// The whole claim of per-class animation: two classes with two sets answer differently on one client. A
// backend-wide answer would hand every class the first one's rows, and every assertion about one class on
// its own would still pass.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetPerClassTest,
	"CrowdySDK.Mass.AnimSetResolutionAnswersPerClass", CrowdyMassAnimSetResolutionTestFlags)
bool FCrowdyMassAnimSetPerClassTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());

	// Deliberately different row orders, with one name only one of them authors, so a name resolving alike
	// for both classes cannot be a coincidence of identical tables.
	const TStrongObjectPtr<UCrowdyMassAnimSet> SetA(
		FCrowdyMassAnimSetResolutionTestFixture::MakeAnimSet({TEXT("Dead"), TEXT("Mounted")}));
	const TStrongObjectPtr<UCrowdyMassAnimSet> SetB(
		FCrowdyMassAnimSetResolutionTestFixture::MakeAnimSet({TEXT("Sitting"), TEXT("Dead")}));

	const TStrongObjectPtr<UBlueprintGeneratedClass> ClassA(
		FCrowdyMassAnimSetResolutionTestFixture::MakeCompiledActorClass());
	const TStrongObjectPtr<UMassEntityConfigAsset> ConfigA(
		FCrowdyMassAnimSetResolutionTestFixture::MakeConfig(/*bWithAnimationTrait=*/true,
			TSoftObjectPtr<UCrowdyMassAnimSet>(SetA.Get())));
	Subsystem->SetAuthoredEntityConfigForTest(ClassA.Get(), ConfigA.Get());

	// Reached the other way round on purpose, so the case covers one class per authoring route rather than
	// two of the same.
	const TStrongObjectPtr<UBlueprintGeneratedClass> ClassB(
		FCrowdyMassAnimSetResolutionTestFixture::MakeCompiledActorClass());
	FCrowdyMassAnimSetResolutionTestFixture::AddAnimSetComponent(ClassB.Get(),
		TSoftObjectPtr<UCrowdyMassAnimSet>(SetB.Get()));

	TestSamePtr(TEXT("the configured class gets its own set"),
		Subsystem->GetRemoteEntityAnimSet(ClassA.Get()), static_cast<const UCrowdyMassAnimSet*>(SetA.Get()));
	TestSamePtr(TEXT("the zero-config class gets its own, not the other one's"),
		Subsystem->GetRemoteEntityAnimSet(ClassB.Get()), static_cast<const UCrowdyMassAnimSet*>(SetB.Get()));

	// A pose name is a row position, so the same name is a different id per class. This is the assertion a
	// class-less lookup fails.
	TestEqual(TEXT("'Dead' is the first row of one class's set"),
		Subsystem->FindPoseId(ClassA.Get(), TEXT("Dead")), 1);
	TestEqual(TEXT("and the second row of the other's"),
		Subsystem->FindPoseId(ClassB.Get(), TEXT("Dead")), 2);

	// And a name one class authors and the other does not resolves for one and clears for the other, rather
	// than resolving for both out of whichever set answered first.
	TestEqual(TEXT("a name only the first class authors resolves for it"),
		Subsystem->FindPoseId(ClassA.Get(), TEXT("Mounted")), 2);
	TestEqual(TEXT("and resolves to no pose for the class that does not"),
		Subsystem->FindPoseId(ClassB.Get(), TEXT("Mounted")), 0);

	TestEqual(TEXT("a pose asked for with no class resolves to no pose"),
		Subsystem->FindPoseId(nullptr, TEXT("Dead")), 0);

	return true;
}

// A cached MISS is the half that is easy to leave out, and it is the one that matters: a class that
// resolved to nothing, or a name that resolved to no row, would otherwise keep answering nothing for the
// life of the world even after the very thing a warning asked for was authored.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetCacheInvalidationTest,
	"CrowdySDK.Mass.AnimSetResolutionDropsCachedMissesWhenASetCanChange", CrowdyMassAnimSetResolutionTestFlags)
bool FCrowdyMassAnimSetCacheInvalidationTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(
		FCrowdyMassAnimSetResolutionTestFixture::MakeAnimSet({TEXT("Dead")}));

	const TStrongObjectPtr<UBlueprintGeneratedClass> LateClass(
		FCrowdyMassAnimSetResolutionTestFixture::MakeCompiledActorClass());
	UCrowdyMassAnimSetComponent* Component = FCrowdyMassAnimSetResolutionTestFixture::AddAnimSetComponent(
		LateClass.Get(), FCrowdyMassAnimSetResolutionTestFixture::NoAnimSet());

	ECrowdyMassAnimSetSource Source = ECrowdyMassAnimSetSource::ConfigTrait;

	TestNull(TEXT("a class whose component names nothing resolves nothing"),
		Subsystem->FindRemoteEntityAnimSet(LateClass.Get(), Source));
	TestEqual(TEXT("and says nothing declares a set"),
		static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::Undeclared));
	TestEqual(TEXT("so no pose name resolves for it either"),
		Subsystem->FindPoseId(LateClass.Get(), TEXT("Dead")), 0);

	// Authoring the set the warning asked for, with nothing yet told to drop the answer.
	Component->AnimSet = TSoftObjectPtr<UCrowdyMassAnimSet>(AnimSet.Get());

	// The miss is genuinely CACHED, which is what makes the drop below load-bearing rather than incidental:
	// without this assertion the case would pass just as well if nothing were ever cached at all.
	TestNull(TEXT("the cached miss still answers until something drops it"),
		Subsystem->FindRemoteEntityAnimSet(LateClass.Get(), Source));
	TestEqual(TEXT("still reported as undeclared"),
		static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::Undeclared));

	Subsystem->InvalidateAnimSetCachesForTest();

	TestSamePtr(TEXT("dropping the answers lets the newly authored set be seen"),
		Subsystem->FindRemoteEntityAnimSet(LateClass.Get(), Source),
		static_cast<const UCrowdyMassAnimSet*>(AnimSet.Get()));
	TestEqual(TEXT("from the class's own component"),
		static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::ClassComponent));

	// The pose id cache is derived from the set cache, so it has to go with it. A pose name that missed
	// while the class had no set would otherwise keep answering 0 against a set that now authors it.
	TestEqual(TEXT("and the pose name that missed now resolves"),
		Subsystem->FindPoseId(LateClass.Get(), TEXT("Dead")), 1);

	return true;
}

#if WITH_EDITOR
/**
 * Something in the editor actually performs the drop, for the two assets an author edits in place.
 *
 * The case above proves the drop works when it is asked for. This is the other half: editing an animation
 * set or a Mass entity config in the details panel replaces no object, so nothing that watches for replaced
 * objects fires, and without this hook acting on the very warning this subsystem just printed would change
 * nothing for the rest of the session.
 *
 * What a headless case cannot reach is the registration itself, which needs the subsystem installed in a
 * world. What it can reach is which objects the hook acts on, and that is where the drop is either
 * performed or quietly missed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetLiveEditInvalidationTest,
	"CrowdySDK.Mass.AnimSetResolutionDropsAnswersWhenAnAssetIsEditedInPlace", CrowdyMassAnimSetResolutionTestFlags)
bool FCrowdyMassAnimSetLiveEditInvalidationTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(
		FCrowdyMassAnimSetResolutionTestFixture::MakeAnimSet({TEXT("Dead")}));

	const TStrongObjectPtr<UBlueprintGeneratedClass> DeclaringClass(
		FCrowdyMassAnimSetResolutionTestFixture::MakeCompiledActorClass());
	UCrowdyMassAnimSetComponent* Component = FCrowdyMassAnimSetResolutionTestFixture::AddAnimSetComponent(
		DeclaringClass.Get(), FCrowdyMassAnimSetResolutionTestFixture::NoAnimSet());

	// Editing the animation set. The class resolves to nothing first, which is the miss the hook has to
	// drop; the component is then filled in the way the details panel fills it.
	{
		const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
			FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());

		ECrowdyMassAnimSetSource Source = ECrowdyMassAnimSetSource::ConfigTrait;
		TestNull(TEXT("the class resolves nothing to begin with"),
			Subsystem->FindRemoteEntityAnimSet(DeclaringClass.Get(), Source));

		Component->AnimSet = TSoftObjectPtr<UCrowdyMassAnimSet>(AnimSet.Get());

		// An unrelated object must not drop anything, or the hook would be emptying these caches on every
		// property edit anywhere in the editor.
		Subsystem->HandleObjectPropertyChangedForTest(DeclaringClass.Get());
		TestNull(TEXT("editing something that is neither asset drops nothing"),
			Subsystem->FindRemoteEntityAnimSet(DeclaringClass.Get(), Source));

		Subsystem->HandleObjectPropertyChangedForTest(AnimSet.Get());
		TestSamePtr(TEXT("editing an animation set drops the answers"),
			Subsystem->FindRemoteEntityAnimSet(DeclaringClass.Get(), Source),
			static_cast<const UCrowdyMassAnimSet*>(AnimSet.Get()));
	}

	// Editing the Mass entity config, which is the other asset a class's answer is made of: adding the
	// animation trait a warning asked for has to be visible in the same session.
	{
		const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
			FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());
		const TStrongObjectPtr<UMassEntityConfigAsset> Config(
			FCrowdyMassAnimSetResolutionTestFixture::MakeConfig(/*bWithAnimationTrait=*/false,
				FCrowdyMassAnimSetResolutionTestFixture::NoAnimSet()));
		Subsystem->SetAuthoredEntityConfigForTest(DeclaringClass.Get(), Config.Get());

		ECrowdyMassAnimSetSource Source = ECrowdyMassAnimSetSource::ConfigTrait;
		Subsystem->FindRemoteEntityAnimSet(DeclaringClass.Get(), Source);
		TestEqual(TEXT("the config installs no animation trait, so nothing renders the class's set"),
			static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::ClassComponentNotRendered));

		UCrowdyMassAnimationTrait* Trait = NewObject<UCrowdyMassAnimationTrait>(Config.Get(), NAME_None, RF_Transient);
		Trait->AnimSet = TSoftObjectPtr<UCrowdyMassAnimSet>(AnimSet.Get());
		Config->GetMutableConfig().AddTrait(*Trait);

		Subsystem->HandleObjectPropertyChangedForTest(Config.Get());

		Subsystem->FindRemoteEntityAnimSet(DeclaringClass.Get(), Source);
		TestEqual(TEXT("and adding the trait is seen in the same session"),
			static_cast<uint8>(Source), static_cast<uint8>(ECrowdyMassAnimSetSource::ConfigTrait));
	}

	return true;
}
#endif // WITH_EDITOR

/**
 * An action id is a row position too, so the same id names a different clip per class. An actor plays what
 * its OWN class authored, which is what stops an actor of one class playing another class's animation for
 * the same event.
 *
 * This reaches the class-keyed resolution the actor path now runs on, and deliberately not the actor path
 * itself: with a subsystem in the world, UCrowdyMassActionLibrary reads the class off the mesh's owning actor
 * and asks it, which no headless case can build. That hop is covered by the two-client gate; the no-subsystem
 * route is the next case.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassAnimSetActionIdPerClassTest,
	"CrowdySDK.Mass.AnimSetActionIdResolvesTheAskingClassesOwnRow", CrowdyMassAnimSetResolutionTestFlags)
bool FCrowdyMassAnimSetActionIdPerClassTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UCrowdyMassEntitySubsystem> Subsystem(
		FCrowdyMassAnimSetResolutionTestFixture::MakeSubsystem());

	const TStrongObjectPtr<UCrowdyMassAnimSet> SetA(
		FCrowdyMassAnimSetResolutionTestFixture::MakeAnimSet({}, /*ActionCount=*/1));
	const TStrongObjectPtr<UCrowdyMassAnimSet> SetB(
		FCrowdyMassAnimSetResolutionTestFixture::MakeAnimSet({}, /*ActionCount=*/1));

	const TStrongObjectPtr<UBlueprintGeneratedClass> ClassA(
		FCrowdyMassAnimSetResolutionTestFixture::MakeCompiledActorClass());
	FCrowdyMassAnimSetResolutionTestFixture::AddAnimSetComponent(ClassA.Get(),
		TSoftObjectPtr<UCrowdyMassAnimSet>(SetA.Get()));

	const TStrongObjectPtr<UBlueprintGeneratedClass> ClassB(
		FCrowdyMassAnimSetResolutionTestFixture::MakeCompiledActorClass());
	FCrowdyMassAnimSetResolutionTestFixture::AddAnimSetComponent(ClassB.Get(),
		TSoftObjectPtr<UCrowdyMassAnimSet>(SetB.Get()));

	const UCrowdyMassAnimSet* ResolvedA = Subsystem->GetRemoteEntityAnimSet(ClassA.Get());
	const UCrowdyMassAnimSet* ResolvedB = Subsystem->GetRemoteEntityAnimSet(ClassB.Get());
	if (!TestNotNull(TEXT("the first class resolves a set"), ResolvedA)
		|| !TestNotNull(TEXT("the second class resolves a set"), ResolvedB))
	{
		return false;
	}

	UAnimSequence* ClipA = ResolvedA->FindActionSequence(1);
	UAnimSequence* ClipB = ResolvedB->FindActionSequence(1);

	TestNotNull(TEXT("action 1 names a clip for the first class"), ClipA);
	TestNotNull(TEXT("action 1 names a clip for the second class"), ClipB);
	TestNotSamePtr(TEXT("and it is a different clip for each, which one set for the whole crowd could not do"),
		ClipA, ClipB);

	return true;
}

/**
 * A map whose profile does not select the Mass backend has no entity subsystem, and an actor on it still
 * plays the action its own class authored. An editor world stands in for that map because it never holds the
 * subsystem either.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassActionLibraryResolvesWithoutSubsystemTest,
	"CrowdySDK.Mass.ActionLibrary.ResolvesTheClassSetWithNoMassSubsystem", CrowdyMassAnimSetResolutionTestFlags)
bool FCrowdyMassActionLibraryResolvesWithoutSubsystemTest::RunTest(const FString& Parameters)
{
	struct FScopedBareWorld
	{
		UWorld* World = nullptr;

		FScopedBareWorld()
		{
			const FString MapName = FString::Printf(TEXT("CrowdyMassActionLibrary_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false, FName(*MapName),
				CreatePackage(*(TEXT("/Temp/") + MapName)));
			GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
		}

		~FScopedBareWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	const TStrongObjectPtr<UCrowdyMassAnimSet> AnimSet(
		FCrowdyMassAnimSetResolutionTestFixture::MakeAnimSet({}, /*ActionCount=*/1));

	// The native class's default component is what the class declares, so the set is written there and put back.
	UCrowdyMassAnimSetComponent* DefaultComponent = GetMutableDefault<ACrowdyMassActionLibraryTestActor>()->AnimSetComponent;
	if (!TestNotNull(TEXT("the test actor class carries its default component"), DefaultComponent))
	{
		return false;
	}

	struct FRestoreAnimSet
	{
		UCrowdyMassAnimSetComponent* Component;
		TSoftObjectPtr<UCrowdyMassAnimSet> Saved;

		~FRestoreAnimSet()
		{
			Component->AnimSet = Saved;
		}
	} Restore{DefaultComponent, DefaultComponent->AnimSet};

	DefaultComponent->AnimSet = TSoftObjectPtr<UCrowdyMassAnimSet>(AnimSet.Get());

	FScopedBareWorld Map;
	if (!TestNull(TEXT("the stand-in world holds no entity subsystem"), Map.World->GetSubsystem<UCrowdyMassEntitySubsystem>()))
	{
		return false;
	}

	AActor* Declaring = NewObject<ACrowdyMassActionLibraryTestActor>(Map.World->PersistentLevel, NAME_None, RF_Transient);
	AActor* Silent = NewObject<AActor>(Map.World->PersistentLevel, AActor::StaticClass(), NAME_None, RF_Transient);
	if (!TestSamePtr(TEXT("the actor belongs to the stand-in world"), Declaring->GetWorld(), Map.World))
	{
		return false;
	}

	UAnimSequence* Clip = UCrowdyMassActionLibrary::FindCrowdyMassActionSequence(Declaring, 1);
	TestNotNull(TEXT("an actor whose class names a set resolves its action with no subsystem"), Clip);
	TestSamePtr(TEXT("and it is the clip that class's set authored"), Clip, AnimSet->FindActionSequence(1));
	TestNull(TEXT("an actor whose class names no set resolves nothing"),
		UCrowdyMassActionLibrary::FindCrowdyMassActionSequence(Silent, 1));

	return true;
}

#endif
