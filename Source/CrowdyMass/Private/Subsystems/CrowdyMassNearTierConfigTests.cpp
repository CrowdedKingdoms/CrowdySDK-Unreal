#include "Derivation/CrowdyMassNearTierDerivation.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Actors/CrowdyMassNearTierActor.h"
#include "Components/ActorComponent.h"
#include "Components/CrowdyMassNearTierComponent.h"
#include "Components/CrowdyMassNearTierShellComponent.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

// Whether a class gets a near tier at all is decided here, once, from the class alone: a config is built
// with an actor at its nearest level only for a class that declares a shell, and every other class keeps
// the crowd row table it has always had. This is the gate that stops promotion turning itself on for every
// class in a project, so the two answers are pinned against each other rather than each on its own.
//
// The route built here is the one a game actually uses: a shell component added in the Blueprint editor,
// which never reaches a class default object and is the route a default-object read alone would miss.
static constexpr EAutomationTestFlags CrowdyMassNearTierConfigTestFlags =
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

struct FCrowdyMassNearTierConfigTestFixture
{
	/**
	 * Stands in for a COMPILED Blueprint class, which is the only kind derivation answers for: it carries
	 * the parent's constructor, is linked against that parent so it has the parent's size, and then has a
	 * default object built. Building a default object before the link allocates a zero-sized object and
	 * runs an actor's constructor over it, which corrupts the heap silently.
	 */
	static UBlueprintGeneratedClass* MakeCompiledActorClass()
	{
		UBlueprintGeneratedClass* GeneratedClass =
			NewObject<UBlueprintGeneratedClass>(GetTransientPackage(), NAME_None, RF_Transient);
		UClass* ParentClass = AActor::StaticClass();
		GeneratedClass->ClassWithin = ParentClass->ClassWithin;
		GeneratedClass->ClassConstructor = ParentClass->ClassConstructor;
		GeneratedClass->ClassCastFlags |= ParentClass->ClassCastFlags;
		GeneratedClass->SetSuperStruct(ParentClass);
		GeneratedClass->Bind();
		GeneratedClass->StaticLink(/*bRelinkExistingProperties=*/true);
		GeneratedClass->GetDefaultObject(/*bCreateIfNeeded=*/true);
		return GeneratedClass;
	}

	// The shape any component added in the Blueprint editor takes: a construction script node holding a
	// component template, with no matching component anywhere on a default object.
	static USimpleConstructionScript& GetOrAddConstructionScript(UBlueprintGeneratedClass* GeneratedClass)
	{
		if (!GeneratedClass->SimpleConstructionScript)
		{
			GeneratedClass->SimpleConstructionScript =
				NewObject<USimpleConstructionScript>(GeneratedClass, NAME_None, RF_Transient);
		}

		return *GeneratedClass->SimpleConstructionScript;
	}

	static void AddComponentNode(UBlueprintGeneratedClass* GeneratedClass, UActorComponent* Template)
	{
		USimpleConstructionScript& Script = GetOrAddConstructionScript(GeneratedClass);

		USCS_Node* Node = NewObject<USCS_Node>(&Script, NAME_None, RF_Transient);
		Node->ComponentClass = Template->GetClass();
		Node->ComponentTemplate = Template;
		Script.AddNode(Node);
	}

	static void AddShellNode(UBlueprintGeneratedClass* GeneratedClass, UClass* NearTierActorClass)
	{
		UCrowdyMassNearTierShellComponent* Template =
			NewObject<UCrowdyMassNearTierShellComponent>(GeneratedClass, NAME_None, RF_Transient);
		if (NearTierActorClass)
		{
			Template->NearTierActorClass = NearTierActorClass;
		}

		AddComponentNode(GeneratedClass, Template);
	}

	// The route a game's own actor class opts into being promoted to: the view component added in the
	// Blueprint editor, on a class that derives from nothing of this plugin's.
	static void AddViewNode(UBlueprintGeneratedClass* GeneratedClass)
	{
		AddComponentNode(GeneratedClass,
			NewObject<UCrowdyMassNearTierComponent>(GeneratedClass, NAME_None, RF_Transient));
	}
};

/**
 * A class that declares a shell gets one; a class that does not, does not.
 *
 * Promotion costs a real actor per entity, and the whole reason it is declared per class is that a project
 * which never asks for it pays nothing and behaves exactly as it did before. A derivation that answered
 * with a shell for any class would turn the near tier on for every crowd in the project at once, and would
 * do it without anything being authored.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierPromotionIsOptedIntoPerClassTest,
	"CrowdySDK.Mass.NearTierPromotionIsOptedIntoPerClass", CrowdyMassNearTierConfigTestFlags)
bool FCrowdyMassNearTierPromotionIsOptedIntoPerClassTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UBlueprintGeneratedClass> DeclaringClass(
		FCrowdyMassNearTierConfigTestFixture::MakeCompiledActorClass());
	FCrowdyMassNearTierConfigTestFixture::AddShellNode(DeclaringClass.Get(), ACrowdyMassNearTierActor::StaticClass());

	const FCrowdyMassDerivedNearTierShell Declared = FCrowdyMassNearTierDerivation::FromClass(DeclaringClass.Get());

	TestTrue(TEXT("a class that declares a shell answers with one"), Declared.IsValid());
	TestTrue(TEXT("and the component it was read off was seen"), Declared.bSawComponent);
	TestEqual(TEXT("the shell is the actor class that was assigned, not some other one"),
		Declared.NearTierActorClass.ToString(), ACrowdyMassNearTierActor::StaticClass()->GetPathName());
	TestEqual(TEXT("and it is reported as having been added in the Blueprint, which is the route it took"),
		static_cast<int32>(Declared.Route), static_cast<int32>(ECrowdyMassNearTierRoute::BlueprintComponent));
	TestTrue(TEXT("an answer that succeeded has nothing to explain"), Declared.Reason.IsEmpty());
	TestFalse(TEXT("and names the component it was read off"), Declared.ComponentName.IsEmpty());

	// The other half of the same claim. An Actor carries no shell component of any kind, and is what every
	// class in a project that has never heard of the near tier looks like.
	const FCrowdyMassDerivedNearTierShell Undeclared = FCrowdyMassNearTierDerivation::FromClass(AActor::StaticClass());

	TestFalse(TEXT("a class that declares no shell answers with none, so its entities stay crowd rows"),
		Undeclared.IsValid());
	TestFalse(TEXT("no shell component was seen on it"), Undeclared.bSawComponent);
	TestEqual(TEXT("and no route is named, since a route recorded for nothing would name a component that supplied no shell"),
		static_cast<int32>(Undeclared.Route), static_cast<int32>(ECrowdyMassNearTierRoute::None));
	TestTrue(TEXT("the reason names the class it is about"),
		Undeclared.Reason.Contains(AActor::StaticClass()->GetPathName()));
	TestTrue(TEXT("and says the component is what is missing"),
		Undeclared.Reason.Contains(TEXT("no Crowdy Mass Near Tier Shell component")));

	return true;
}

/**
 * A component with an empty slot is a different fault from no component at all.
 *
 * One is a slot to fill in and the other is a component to add, and both arrive as the same null class, so
 * the only thing that can tell an author which one they have is this. Merging them would send half of them
 * looking for a component they already have.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierEmptyShellSlotIsNotAMissingComponentTest,
	"CrowdySDK.Mass.NearTierEmptyShellSlotIsNotAMissingComponent", CrowdyMassNearTierConfigTestFlags)
bool FCrowdyMassNearTierEmptyShellSlotIsNotAMissingComponentTest::RunTest(const FString& Parameters)
{
	const TStrongObjectPtr<UBlueprintGeneratedClass> EmptySlotClass(
		FCrowdyMassNearTierConfigTestFixture::MakeCompiledActorClass());
	FCrowdyMassNearTierConfigTestFixture::AddShellNode(EmptySlotClass.Get(), /*NearTierActorClass=*/nullptr);

	const FCrowdyMassDerivedNearTierShell EmptySlot = FCrowdyMassNearTierDerivation::FromClass(EmptySlotClass.Get());

	TestFalse(TEXT("a shell component with nothing assigned answers with no shell"), EmptySlot.IsValid());
	TestTrue(TEXT("but the component itself was seen"), EmptySlot.bSawComponent);
	TestTrue(TEXT("and the reason says the class is what is missing, not the component"),
		EmptySlot.Reason.Contains(TEXT("no actor class is assigned")));

	const FCrowdyMassDerivedNearTierShell NoComponent = FCrowdyMassNearTierDerivation::FromClass(AActor::StaticClass());
	TestNotEqual(TEXT("the two faults are reported differently rather than as one message reached two ways"),
		EmptySlot.Reason, NoComponent.Reason);

	return true;
}

/**
 * The classes that cannot be answered for at all, kept apart from a class that simply declares nothing.
 *
 * The unfinished class is the one that matters: reading a class default object would BUILD one, and running
 * a class's constructor while it is still forming asserts rather than failing, so refusing before reaching
 * it is what keeps that unreachable.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierShellRefusesUnaskableClassesTest,
	"CrowdySDK.Mass.NearTierShellRefusesUnaskableClasses", CrowdyMassNearTierConfigTestFlags)
bool FCrowdyMassNearTierShellRefusesUnaskableClassesTest::RunTest(const FString& Parameters)
{
	const FCrowdyMassDerivedNearTierShell NoClass = FCrowdyMassNearTierDerivation::FromClass(nullptr);
	TestFalse(TEXT("no class at all answers with no shell"), NoClass.IsValid());
	TestFalse(TEXT("and still says why"), NoClass.Reason.IsEmpty());

	const FCrowdyMassDerivedNearTierShell NotAnActor = FCrowdyMassNearTierDerivation::FromClass(UObject::StaticClass());
	TestFalse(TEXT("a class that is not an Actor has no components to read and answers with no shell"),
		NotAnActor.IsValid());
	TestTrue(TEXT("and says that is what is wrong with it"), NotAnActor.Reason.Contains(TEXT("not an Actor class")));

	// Deliberately given no constructor and no default object, which is what a class part way through
	// compilation looks like.
	const TStrongObjectPtr<UBlueprintGeneratedClass> UnfinishedClass(
		NewObject<UBlueprintGeneratedClass>(GetTransientPackage(), NAME_None, RF_Transient));
	UnfinishedClass->ClassWithin = AActor::StaticClass()->ClassWithin;
	UnfinishedClass->SetSuperStruct(AActor::StaticClass());

	const FCrowdyMassDerivedNearTierShell Unfinished = FCrowdyMassNearTierDerivation::FromClass(UnfinishedClass.Get());
	TestFalse(TEXT("an unfinished class answers with no shell"), Unfinished.IsValid());
	TestTrue(TEXT("and says it is not ready rather than that it declared nothing"),
		Unfinished.Reason.Contains(TEXT("no class default object")));

	return true;
}

/**
 * What a class may be promoted TO is decided by the component it carries, not by the base it derives from.
 *
 * Naming an ordinary character or prop Blueprint is the obvious thing for an author to try, and it produces
 * no visible complaint of its own: the actor is spawned and enabled, placed once, and then stands still
 * while its entity walks away, with its crowd row no longer drawn and the entity no longer hittable. Both
 * halves are pinned here, because a check that accepted anything and a check that accepted nothing both
 * read as green from one side.
 *
 * The cell that separates this question from "is it one of ours" is the third: a game's own actor class
 * that opted in by adding the view component in the Blueprint editor, deriving from nothing of this
 * plugin's. Answering by class instead would refuse it, and refusing it is not a diagnosable failure, it is
 * a class that quietly keeps drawing as crowd rows however near it comes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyMassNearTierShellNeedsTheViewComponentTest,
	"CrowdySDK.Mass.NearTierShellNeedsTheViewComponent", CrowdyMassNearTierConfigTestFlags)
bool FCrowdyMassNearTierShellNeedsTheViewComponentTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("this plugin's own shell carries the view component and can be promoted to"),
		FCrowdyMassNearTierDerivation::CanHostNearTierView(ACrowdyMassNearTierActor::StaticClass()));

	// A compiled Blueprint deriving straight from Actor: the ordinary character or prop an author would
	// reach for, an Actor class in every respect and still not something an entity can be driven through.
	const TStrongObjectPtr<UBlueprintGeneratedClass> PlainActorClass(
		FCrowdyMassNearTierConfigTestFixture::MakeCompiledActorClass());

	TestFalse(TEXT("an ordinary Blueprint actor with no view component is refused"),
		FCrowdyMassNearTierDerivation::CanHostNearTierView(PlainActorClass.Get()));

	// The same class with the view component added in the Blueprint editor, which is how a game opts one of
	// its own classes in. It derives from Actor and from nothing of this plugin's, and it is exactly as
	// promotable as the shell above.
	const TStrongObjectPtr<UBlueprintGeneratedClass> OptedInActorClass(
		FCrowdyMassNearTierConfigTestFixture::MakeCompiledActorClass());
	FCrowdyMassNearTierConfigTestFixture::AddViewNode(OptedInActorClass.Get());

	TestTrue(TEXT("a game's own actor class that added the view component is accepted, whatever it derives from"),
		FCrowdyMassNearTierDerivation::CanHostNearTierView(OptedInActorClass.Get()));

	TestFalse(TEXT("a bare Actor carries no view component either and is refused"),
		FCrowdyMassNearTierDerivation::CanHostNearTierView(AActor::StaticClass()));
	TestFalse(TEXT("and a class that is not an Actor at all"),
		FCrowdyMassNearTierDerivation::CanHostNearTierView(UObject::StaticClass()));
	TestFalse(TEXT("and no class"),
		FCrowdyMassNearTierDerivation::CanHostNearTierView(nullptr));

	// A class part way through compilation has no default object, and building one to answer this question
	// would run an actor constructor over a zero-sized object.
	const TStrongObjectPtr<UBlueprintGeneratedClass> UnfinishedClass(
		NewObject<UBlueprintGeneratedClass>(GetTransientPackage(), NAME_None, RF_Transient));
	UnfinishedClass->ClassWithin = AActor::StaticClass()->ClassWithin;
	UnfinishedClass->SetSuperStruct(AActor::StaticClass());

	TestFalse(TEXT("an unfinished class is refused rather than made to build a default object"),
		FCrowdyMassNearTierDerivation::CanHostNearTierView(UnfinishedClass.Get()));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
