#include "Replication/GameModel/CrowdyEntityClassContainer.h"

// WITH_METADATA as well as the test flag: these cases read the CrowdyContainer tag of a fixture component class,
// and outside the editor the only source of that tag is the baked registry, which excludes every CrowdyContainerTest
// fixture on purpose. They would compile and link into a game target and then fail there, reporting a derivation of
// zero containers as a broken walk.
#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/GCObjectScopeGuard.h"
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership
#include "Replication/GameModel/CrowdyEntityClassContainerTestTypes.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"
#include "UObject/Script.h" // FEditorScriptExecutionGuard

/**
 * A row drawn for somebody else's character has no actor and no components, so the CrowdyContainer components
 * its owner enrolled exist here only as a derivation from the class. These cover that derivation and the one
 * invariant everything after it rests on: the id derived from the class alone has to equal the id the owner
 * minted for the real component, because a mismatch reads a container row that does not exist and says nothing.
 */
namespace
{
	constexpr EAutomationTestFlags CrowdyDerivedContainerTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Distinct helper names so this test TU never collides with the other Game Model test helpers under unity.
	UCrowdyEntitySubsystem* MakeDerivedContainerEntities()
	{
		UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
		Entities->SetLocalPlayerID(FGuid(0x10CA1, 7, 7, 7));
		return Entities;
	}

	// An editor world, so an actor fixture can actually be spawned with its default subobjects built. A worldless
	// NewObject actor has no components at all, which is precisely the difference this file is about.
	struct FCrowdyDerivedContainerWorld
	{
		FEditorScriptExecutionGuard ScriptGuard;
		UWorld* World = nullptr;

		FCrowdyDerivedContainerWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyDerivedContainerWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		template <typename T>
		T* Spawn() { return World->SpawnActor<T>(); }
	};
}

/**
 * SEED PARITY. The whole mechanism rests on this one equality and its failure is completely silent.
 *
 * The owner runs the real actor and enrolls its component through RegisterSubParticipant. An observer has only
 * the class, so it derives the component and the per-instance term from the class chain and mints the id from
 * those. If the two derivations disagree the observer reads a container row by a key nobody wrote, finds
 * nothing, and the row keeps its spawn defaults forever with no error anywhere.
 *
 * Mutating the instance term in GetDerivedComponentContainers (using the SCS template's own name, which is
 * suffixed _GEN_VARIABLE, or the node's declared ComponentClass instead of the template's) turns this red, and
 * so does mutating the SeedClass RegisterSubParticipantAs folds into the seed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDerivedContainerSeedParityTest,
	"CrowdySDK.GameModel.DerivedContainerSeedParity", CrowdyDerivedContainerTestFlags)
bool FCrowdyDerivedContainerSeedParityTest::RunTest(const FString& Parameters)
{
	FCrowdyDerivedContainerWorld TestWorld;
	UCrowdyEntitySubsystem* Entities = MakeDerivedContainerEntities();

	// The owner's side: the real actor, its real component, enrolled through the production call.
	ACrowdyEntityClassComponentOwnerActor* Owner = TestWorld.Spawn<ACrowdyEntityClassComponentOwnerActor>();
	if (!TestNotNull(TEXT("the owner's actor spawned"), Owner)
		|| !TestNotNull(TEXT("and carries its attribute component"), ToRawPtr(Owner->Attributes)))
	{
		return false;
	}

	const FGuid AnchorNetID = Entities->RegisterParticipant(Owner, ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("the anchor enrolled"), AnchorNetID.IsValid()))
	{
		return false;
	}

	const FGuid OwnerMinted = Entities->RegisterSubParticipant(Owner->Attributes, AnchorNetID);
	if (!TestTrue(TEXT("the owner minted an id for its real component"), OwnerMinted.IsValid()))
	{
		return false;
	}

	// The observer's side: the class alone, with no actor and no component anywhere.
	TArray<FCrowdyDerivedComponentContainer> Derived;
	CrowdyEntityClassContainer::GetDerivedComponentContainers(
		ACrowdyEntityClassComponentOwnerActor::StaticClass(), Derived);

	if (!TestEqual(TEXT("the class alone derives exactly one component container"), Derived.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("naming the component's own class"),
		Derived[0].ComponentClass.Get(), static_cast<const UClass*>(UCrowdyGameModelTestComponent::StaticClass()));
	TestEqual(TEXT("and the container type its class declares"), Derived[0].TypeName, FString(TEXT("TestAttributes")));

	const FGuid ObserverDerived = UCrowdyEntitySubsystem::DeriveSubParticipantID(
		AnchorNetID, Derived[0].ComponentClass.Get(), Derived[0].InstanceTerm);

	TestEqual(TEXT("the id derived from the class equals the id the owner minted for the real component"),
		ObserverDerived, OwnerMinted);

	// Stated rather than implied: parity is worthless if the derivation answers the same for everything.
	TestNotEqual(TEXT("and a different anchor derives a different id"), ObserverDerived,
		UCrowdyEntitySubsystem::DeriveSubParticipantID(FGuid(1, 2, 3, 4), Derived[0].ComponentClass.Get(),
			Derived[0].InstanceTerm));
	TestNotEqual(TEXT("and so does a different instance term"), ObserverDerived,
		UCrowdyEntitySubsystem::DeriveSubParticipantID(AnchorNetID, Derived[0].ComponentClass.Get(),
			TEXT("SomeOtherName")));

	return true;
}

/**
 * A class that declares nothing itself still derives the containers its components declare, and one that
 * declares both derives both. The second is what a real player character looks like: its own container plus its
 * attribute component's.
 *
 * Mutating DeriveComponentContainers to stop at the first match (the shape ResolveDefaultEntityComponent has,
 * which this walk is modelled on) turns the two-container case red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDerivedContainerWalksWholeClassTest,
	"CrowdySDK.GameModel.DerivedContainerWalksWholeClass", CrowdyDerivedContainerTestFlags)
bool FCrowdyDerivedContainerWalksWholeClassTest::RunTest(const FString& Parameters)
{
	TArray<FCrowdyDerivedComponentContainer> Derived;

	// A class whose only container is on a component.
	CrowdyEntityClassContainer::GetDerivedComponentContainers(
		ACrowdyEntityClassComponentOwnerActor::StaticClass(), Derived);
	TestEqual(TEXT("a class declaring nothing itself still derives its component's container"), Derived.Num(), 1);

	// A class declaring one itself AND carrying a container component. The actor's own declaration is not a
	// component and is deliberately not reported here: it binds through the participant, not through a stand-in.
	CrowdyEntityClassContainer::GetDerivedComponentContainers(
		ACrowdyEntityClassBothContainersActor::StaticClass(), Derived);
	if (!TestEqual(TEXT("a class declaring its own container still derives its component's"), Derived.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("and that is the component's type, not the actor's"),
		Derived[0].TypeName, FString(TEXT("TestAttributes")));

	// A class with no container anywhere derives nothing at all, rather than everything it happens to carry.
	CrowdyEntityClassContainer::GetDerivedComponentContainers(
		ACrowdyEntityClassPlainActor::StaticClass(), Derived);
	TestEqual(TEXT("a class with no CrowdyContainer component derives nothing"), Derived.Num(), 0);

	// A non-actor class is not a shape this walks at all.
	CrowdyEntityClassContainer::GetDerivedComponentContainers(UCrowdyEntityClassStandIn::StaticClass(), Derived);
	TestEqual(TEXT("a non-actor class derives nothing"), Derived.Num(), 0);

	return true;
}

/**
 * A derivation that could not read the class is not remembered as that class's answer.
 *
 * A class asked about while it is still being loaded has no construction script to read yet, so the walk finds
 * nothing. Caching that would answer "declares no container" for every row of that class for the rest of the
 * process, with no way to invalidate it, and the route diagnostic would name the wrong cause.
 *
 * Mutating GetDerivedComponentContainers to cache unconditionally turns the second half red; mutating away the
 * still-loading guard in DeriveComponentContainers turns the first half red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyIncompleteDerivationIsNotCachedTest,
	"CrowdySDK.GameModel.IncompleteDerivationIsNotCached", CrowdyDerivedContainerTestFlags)
bool FCrowdyIncompleteDerivationIsNotCachedTest::RunTest(const FString& Parameters)
{
	// A Blueprint-generated class standing on the component-owner fixture, exactly as a cooked BP subclass does.
	// Its own CDO is never touched: the walk reads its construction script and then moves to the native ancestor.
	UBlueprintGeneratedClass* StillLoading = NewObject<UBlueprintGeneratedClass>(GetTransientPackage(),
		TEXT("CrowdyDerivedContainerStillLoadingFixture"), RF_Transient);
	StillLoading->SetSuperStruct(ACrowdyEntityClassComponentOwnerActor::StaticClass());
	FGCObjectScopeGuard KeepClass(StillLoading);

	// What the loader leaves on a class it has not finished with.
	StillLoading->SetFlags(RF_NeedPostLoad);

	TArray<FCrowdyDerivedComponentContainer> Derived;
	CrowdyEntityClassContainer::GetDerivedComponentContainers(StillLoading, Derived);
	if (!TestEqual(TEXT("a class that is still loading derives nothing, because there is nothing to read yet"),
		Derived.Num(), 0))
	{
		return false;
	}

	// The loader finishes. Nothing else changes, and nothing invalidates a cache.
	StillLoading->ClearFlags(RF_NeedPostLoad);

	CrowdyEntityClassContainer::GetDerivedComponentContainers(StillLoading, Derived);
	if (!TestEqual(TEXT("asking again derives the container its ancestor carries, so the empty answer was not kept"),
		Derived.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("and it is the component's own type"), Derived[0].TypeName, FString(TEXT("TestAttributes")));

	// The control: the completed answer IS cached, so the case above is about completeness and not about the
	// cache having been removed.
	StillLoading->SetFlags(RF_NeedPostLoad);
	CrowdyEntityClassContainer::GetDerivedComponentContainers(StillLoading, Derived);
	TestEqual(TEXT("a completed derivation is remembered, so a later ask never re-walks the chain"),
		Derived.Num(), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
