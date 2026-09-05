#include "Replication/GameModel/CrowdyEntityClassContainer.h"

// WITH_METADATA as well as the test flag: these cases read the CrowdyContainer tag of a fixture component class,
// and outside the editor the only source of that tag is the baked registry, which excludes every CrowdyContainerTest
// fixture on purpose. They would compile and link into a game target and then fail there, reporting a derivation of
// zero containers as a broken walk.
#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Core/FCrowdyTypeID.h"
#include "Data/CrowdyEntityTypes.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "UObject/GCObjectScopeGuard.h"
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership
#include "Replication/GameModel/CrowdyContainerStandIn.h"
#include "Replication/GameModel/CrowdyEntityClassContainerTestTypes.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"
#include "UObject/Script.h" // FEditorScriptExecutionGuard
#include "UObject/SoftObjectPath.h"
#include "Utils/UCrowdyClassRegistry.h"

/**
 * A row drawn for somebody else's character has no actor and no components, so the CrowdyContainer components
 * its owner enrolled exist here only as a derivation from the class. These cover that derivation and the one
 * invariant everything after it rests on: the id derived from the class alone has to equal the id the owner
 * minted for the real component, because a mismatch reads a container row that does not exist and says nothing.
 *
 * Also here: the pull coalescer, which turns a fight's worth of notifications into one pull per container, and
 * the refusal that keeps a bare container id from binding a row.
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

	UCrowdyGameModelSubsystem* MakeDerivedContainerModel(UCrowdyEntitySubsystem* Entities)
	{
		UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
		Model->SetEntitySubsystemForTest(Entities);
		return Model;
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

	// Puts a class in the class registry the way the startup scan does, and reports whether the round trip an
	// entity record depends on actually works: a record carries only a ClassID. The registry seals once a game
	// instance starts, so a headless run after a PIE session answers false here rather than failing obscurely.
	bool TryRecordDerivedContainerClass(const UClass* Class, uint32& OutClassID)
	{
		UCrowdyClassRegistry* Registry = UCrowdyClassRegistry::Get();
		const FCrowdyClassID ClassID = Registry->GetID(Class);
		Registry->RegisterClass(ClassID, FSoftClassPath(Class));
		OutClassID = ClassID;
		return Registry->Resolve(ClassID).ResolveClass() == Class;
	}

	FCrowdyEntityRecord MakeDerivedContainerRecord(const FGuid& NetID, UObject* Participant, uint32 ClassID)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = NetID;
		Record.OwnerID = FGuid(0x0BADC0DE, 1, 2, 3); // another client simulates it
		Record.Role = ECrowdyRole::RemoteProxy;
		Record.ClassID = ClassID;
		Record.Participant = Participant;
		return Record;
	}

	// A worldless subsystem has no Game API context, so any resolve that is genuinely attempted logs about it.
	void ExpectDerivedContainerApiFailure(FAutomationTestBase& Test)
	{
		Test.AddExpectedErrorPlain(TEXT("Game API"), EAutomationExpectedErrorFlags::Contains, 0);
	}
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
 * The record a stand-in enrolls under has to name the COMPONENT's class, not the stand-in's, because that is
 * what lets the binding read the container declaration off the component. And the binding it produces is
 * read-only: a class-derived binding may say which server row to read and must never create one.
 *
 * Mutating RegisterSubParticipantAs's `Record.ClassID = GetID(SeedClass)` back to the participant's own class
 * turns the type resolution red; mutating away the ClassDerivedBindings.Add in BindParticipantContainer turns
 * the read-only assertion red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStandInBindsComponentContainerReadOnlyTest,
	"CrowdySDK.GameModel.StandInBindsComponentContainerReadOnly", CrowdyDerivedContainerTestFlags)
bool FCrowdyStandInBindsComponentContainerReadOnlyTest::RunTest(const FString& Parameters)
{
	uint32 ComponentClassID = 0;
	if (!TryRecordDerivedContainerClass(UCrowdyGameModelTestComponent::StaticClass(), ComponentClassID))
	{
		AddInfo(TEXT("the class registry is sealed in this process, so the id could not be made resolvable; skipping."));
		return true;
	}

	UCrowdyEntitySubsystem* Entities = MakeDerivedContainerEntities();
	UCrowdyGameModelSubsystem* Model = MakeDerivedContainerModel(Entities);
	ExpectDerivedContainerApiFailure(*this);

	// The anchor: an entity drawn as a row, standing for an actor whose containers live on its components.
	const FGuid AnchorNetID(0xA0, 1, 2, 3);
	UObject* RowStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	uint32 ActorClassID = 0;
	TryRecordDerivedContainerClass(ACrowdyEntityClassComponentOwnerActor::StaticClass(), ActorClassID);
	Entities->RegisterEntity(MakeDerivedContainerRecord(AnchorNetID, RowStandIn, ActorClassID));

	const int32 Enrolled = Model->EnrollDerivedComponentContainers(AnchorNetID, RowStandIn);
	if (!TestEqual(TEXT("one component container was stood in for"), Enrolled, 1))
	{
		return false;
	}

	TArray<FGuid> Subs;
	Model->GetSubParticipants(AnchorNetID, Subs);
	if (!TestEqual(TEXT("and it is tracked under its anchor"), Subs.Num(), 1))
	{
		return false;
	}

	const FCrowdyEntityRecord* SubRecord = Entities->FindRecord(Subs[0]);
	if (!TestNotNull(TEXT("the stand-in enrolled a record"), SubRecord))
	{
		return false;
	}
	TestEqual(TEXT("the record names the COMPONENT's class, which is what resolves the container"),
		static_cast<int64>(SubRecord->ClassID), static_cast<int64>(ComponentClassID));
	TestEqual(TEXT("and it points back at the anchor, which is the only way back from a hashed id"),
		SubRecord->AnchorNetID, AnchorNetID);

	// The binding that record produces.
	Model->HandleEntityRegisteredForTest(Subs[0]);

	FString PendingType;
	if (!TestTrue(TEXT("the sub-participant resolved a container type from the component class"),
		Model->TryGetPendingModelEntityTypeForTest(Subs[0], PendingType)))
	{
		return false;
	}
	TestEqual(TEXT("which is the component's own declaration"), PendingType, FString(TEXT("TestAttributes")));
	TestTrue(TEXT("the binding is recorded as class-derived"), Model->IsClassDerivedBindingForTest(Subs[0]));
	TestFalse(TEXT("so it can only ever read the owner's row, never create one"),
		Model->IsAuthoritativeToCreateForTest(Subs[0]));

	return true;
}

/**
 * A row that goes takes every stand-in it enrolled with it, and the registry ends up holding exactly what it
 * held before the row was drawn.
 *
 * Each stand-in holds a record that keeps a container bound. Left behind, they keep containers bound to objects
 * that receive nothing, and their ids stay claimed, so a later mint for the same row is refused as a collision.
 * The count returning is the assertion rather than the individual records, because a leak of one here is a leak
 * of thousands in a crowd.
 *
 * Mutating away the SubParticipantsByAnchor drain in HandleEntityUnregistered turns this red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyAnchorTeardownReleasesStandInsTest,
	"CrowdySDK.GameModel.AnchorTeardownReleasesStandIns", CrowdyDerivedContainerTestFlags)
bool FCrowdyAnchorTeardownReleasesStandInsTest::RunTest(const FString& Parameters)
{
	uint32 ActorClassID = 0;
	if (!TryRecordDerivedContainerClass(ACrowdyEntityClassBothContainersActor::StaticClass(), ActorClassID))
	{
		AddInfo(TEXT("the class registry is sealed in this process, so the id could not be made resolvable; skipping."));
		return true;
	}

	UCrowdyEntitySubsystem* Entities = MakeDerivedContainerEntities();
	UCrowdyGameModelSubsystem* Model = MakeDerivedContainerModel(Entities);
	Model->BindEntityLifecycleForTest(Entities);
	ExpectDerivedContainerApiFailure(*this);

	const int32 MappingsBefore = Entities->GetParticipantMappingCountForTest();

	const FGuid AnchorNetID(0xA2, 1, 2, 3);
	UObject* RowStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	Entities->RegisterEntity(MakeDerivedContainerRecord(AnchorNetID, RowStandIn, ActorClassID));

	if (!TestEqual(TEXT("the row stood in for its class's component container"),
		Model->EnrollDerivedComponentContainers(AnchorNetID, RowStandIn), 1))
	{
		return false;
	}

	TArray<FGuid> Subs;
	Model->GetSubParticipants(AnchorNetID, Subs);
	if (!TestEqual(TEXT("which is tracked under the anchor"), Subs.Num(), 1))
	{
		return false;
	}
	Model->BindEntityContainerForTest(Subs[0], TEXT("container-attributes"));
	Model->BindEntityContainerForTest(AnchorNetID, TEXT("container-character"));

	TestNotEqual(TEXT("the registry grew while the row was drawn"),
		Entities->GetParticipantMappingCountForTest(), MappingsBefore);

	// The row goes.
	Entities->UnregisterEntity(AnchorNetID);

	TestNull(TEXT("the row's own record is gone"), Entities->FindRecord(AnchorNetID));
	TestNull(TEXT("and so is the stand-in's"), Entities->FindRecord(Subs[0]));

	FString StillBound;
	TestFalse(TEXT("so no container is left bound to an object that receives nothing"),
		Model->TryGetContainerId(Subs[0], StillBound));
	TestFalse(TEXT("for the row either"), Model->TryGetContainerId(AnchorNetID, StillBound));

	TestEqual(TEXT("and the registry holds exactly what it held before the row was drawn"),
		Entities->GetParticipantMappingCountForTest(), MappingsBefore);

	TArray<FGuid> SubsAfter;
	Model->GetSubParticipants(AnchorNetID, SubsAfter);
	TestEqual(TEXT("with nothing left tracked under the anchor"), SubsAfter.Num(), 0);

	// The strong hold is what keeps a stand-in alive, so teardown dropping it is the only thing bounding that map.
	// Without this the release half of the hold has no gate: deleting it leaves every other assertion green while
	// each retained stand-in also pins the avatar it is outered to, defeating both the avatar cap and the grace
	// window as memory bounds.
	TestEqual(TEXT("and the strong hold on the stand-in is released"),
		Model->GetHeldContainerStandInCountForTest(), 0);

	return true;
}

/**
 * Values pulled for a component container have to reach whoever holds the ENTITY.
 *
 * A sub-participant's id is a one-way hash of its anchor's, so a holder of the entity has never seen it and
 * answers "unknown" for it, which the relay reads as a final refusal and drops the change with nothing said.
 * Mutating ResolveSubscriberEntityID to return NetID unconditionally turns this red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySubRecordResolvesToAnchorTest,
	"CrowdySDK.GameModel.SubRecordResolvesToAnchor", CrowdyDerivedContainerTestFlags)
bool FCrowdySubRecordResolvesToAnchorTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeDerivedContainerEntities();
	UCrowdyGameModelSubsystem* Model = MakeDerivedContainerModel(Entities);

	const FGuid AnchorNetID(0xA1, 1, 2, 3);
	UObject* RowStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	Entities->RegisterEntity(MakeDerivedContainerRecord(AnchorNetID, RowStandIn, CROWDY_INVALID_CLASS_ID));

	UCrowdyContainerStandIn* ComponentStandIn = NewObject<UCrowdyContainerStandIn>(GetTransientPackage());
	ComponentStandIn->RepresentedClass = UCrowdyGameModelTestComponent::StaticClass();
	ComponentStandIn->InstanceTerm = TEXT("CrowdyTestAttributes");

	const FGuid SubNetID = Entities->RegisterSubParticipantAs(ComponentStandIn, AnchorNetID,
		UCrowdyGameModelTestComponent::StaticClass(), TEXT("CrowdyTestAttributes"));
	if (!TestTrue(TEXT("the stand-in enrolled"), SubNetID.IsValid()))
	{
		return false;
	}
	TestNotEqual(TEXT("under an id of its own, which is the reason the anchor has to be resolvable"),
		SubNetID, AnchorNetID);

	TestEqual(TEXT("a subscriber is addressed by the anchor for a sub-participant's values"),
		Model->ResolveSubscriberEntityIDForTest(SubNetID), AnchorNetID);
	TestEqual(TEXT("while a top-level entity is addressed by its own id"),
		Model->ResolveSubscriberEntityIDForTest(AnchorNetID), AnchorNetID);

	return true;
}

/**
 * K notifications for one container between drains are ONE pull, and the pull addresses whichever entity holds
 * the container at the LATEST notification.
 *
 * Dropping duplicates is the easy half. Keeping the latest is the half that matters: a container rebound to a
 * different entity mid-window would otherwise have its values applied to the entity that let it go, and a bar
 * would read a value that belongs to somebody else. Mutating the map write in EnqueueRefreshPull to keep the
 * first entry (a FindOrAdd that does not overwrite) turns the latest-wins assertion red while leaving the
 * one-pull assertion green, which is why both are here.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRefreshPullsCoalesceKeepingLatestTest,
	"CrowdySDK.GameModel.RefreshPullsCoalesceKeepingLatest", CrowdyDerivedContainerTestFlags)
bool FCrowdyRefreshPullsCoalesceKeepingLatestTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeDerivedContainerEntities();
	UCrowdyGameModelSubsystem* Model = MakeDerivedContainerModel(Entities);
	// The drain at the end issues the pulls it gathered, and a worldless subsystem has no Game API context.
	ExpectDerivedContainerApiFailure(*this);

	const FGuid FirstHolder(0xB0, 1, 2, 3);
	const FGuid LaterHolder(0xB1, 1, 2, 3);
	const FGuid OtherEntity(0xB2, 1, 2, 3);
	const FString Container(TEXT("container-fight"));
	const FString OtherContainer(TEXT("container-other"));

	Model->BindEntityContainerForTest(FirstHolder, Container);
	Model->BindEntityContainerForTest(OtherEntity, OtherContainer);

	// A fight's worth of changes to one container.
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Model->HandleModelChangedByContainer(Container);
	}
	Model->HandleModelChangedByContainer(OtherContainer);

	TestEqual(TEXT("five notifications for one container are one waiting pull, and the other container is its own"),
		Model->GetPendingRefreshPullCountForTest(), 2);

	FGuid Target;
	if (!TestTrue(TEXT("the busy container is waiting to be pulled"),
		Model->TryGetPendingRefreshPullTargetForTest(Container, Target)))
	{
		return false;
	}
	TestEqual(TEXT("for the entity that holds it"), Target, FirstHolder);

	// The container moves to a different entity, and another notification arrives before the drain.
	Model->UnbindEntityContainerForTest(FirstHolder);
	Model->BindEntityContainerForTest(LaterHolder, Container);
	Model->HandleModelChangedByContainer(Container);

	TestEqual(TEXT("it is still one waiting pull for that container"),
		Model->GetPendingRefreshPullCountForTest(), 2);
	if (!TestTrue(TEXT("still waiting"), Model->TryGetPendingRefreshPullTargetForTest(Container, Target)))
	{
		return false;
	}
	TestEqual(TEXT("and it now addresses the entity holding the container at the LATEST notification"),
		Target, LaterHolder);

	// Draining empties the window rather than leaving entries to be pulled twice.
	Model->DrainRefreshPullsForTest();
	TestEqual(TEXT("the drain clears what it pulled"), Model->GetPendingRefreshPullCountForTest(), 0);

	return true;
}

/**
 * A notification naming only a container id must never bind a row that has never bound.
 *
 * The reverse map is populated AT BIND TIME, so a container id cannot be matched to a row that has not bound
 * yet: binding off one would need the row to already have what the bind is for. The only correct answer is to
 * leave it alone. Mutating HandleModelChangedByContainer to bind or to enqueue anything on the unbound path
 * turns this red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyContainerHintNeverBindsUnboundRowTest,
	"CrowdySDK.GameModel.ContainerHintNeverBindsUnboundRow", CrowdyDerivedContainerTestFlags)
bool FCrowdyContainerHintNeverBindsUnboundRowTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeDerivedContainerEntities();
	UCrowdyGameModelSubsystem* Model = MakeDerivedContainerModel(Entities);

	// A row that exists and has never bound anything.
	const FGuid UnboundNetID(0xC0, 1, 2, 3);
	UObject* RowStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	Entities->RegisterEntity(MakeDerivedContainerRecord(UnboundNetID, RowStandIn, CROWDY_INVALID_CLASS_ID));

	const int32 ResolvesBefore = Model->GetContainerResolveStartCountForTest();
	Model->HandleModelChangedByContainer(TEXT("container-nobody-here-holds"));

	FString BoundContainer;
	TestFalse(TEXT("the row is still unbound"), Model->TryGetContainerId(UnboundNetID, BoundContainer));
	TestEqual(TEXT("nothing was queued to pull for a container no row holds"),
		Model->GetPendingRefreshPullCountForTest(), 0);
	TestEqual(TEXT("and no container resolve was started, so the hint spent nothing"),
		Model->GetContainerResolveStartCountForTest(), ResolvesBefore);

	FString PendingType;
	TestFalse(TEXT("and the row was not even queued to resolve one"),
		Model->TryGetPendingModelEntityTypeForTest(UnboundNetID, PendingType));

	return true;
}

/**
 * A stand-in has to SURVIVE A GARBAGE COLLECTION, and nothing but this subsystem holds one.
 *
 * The registry stores its participant in a non-UPROPERTY weak pointer, and an Outer is an edge from the inner to
 * the outer and never the reverse, so being outered to the row's avatar keeps nothing alive. A collected stand-in
 * leaves its record and its container binding behind: the entity still looks bound, every later change still
 * spends a Game API pull, the completion resolves the participant to null and drops the state, and the surviving
 * record makes the re-mint guard skip its own replacement forever. Health freezes about a minute into play.
 *
 * The collection is the test. Every case that came before this one passed while the defect shipped, because none
 * of them ran a GC. Mutating away the ContainerStandIns.Add in EnrollDerivedComponentContainers turns this red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStandInSurvivesGarbageCollectionTest,
	"CrowdySDK.GameModel.StandInSurvivesGarbageCollection", CrowdyDerivedContainerTestFlags)
bool FCrowdyStandInSurvivesGarbageCollectionTest::RunTest(const FString& Parameters)
{
	uint32 ActorClassID = 0;
	if (!TryRecordDerivedContainerClass(ACrowdyEntityClassComponentOwnerActor::StaticClass(), ActorClassID))
	{
		AddInfo(TEXT("the class registry is sealed in this process, so the id could not be made resolvable; skipping."));
		return true;
	}

	UCrowdyEntitySubsystem* Entities = MakeDerivedContainerEntities();
	UCrowdyGameModelSubsystem* Model = MakeDerivedContainerModel(Entities);
	UObject* RowStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());

	// No registration handler is wired here on purpose: this case is about the OBJECT surviving, and binding a
	// container would only add a round trip with nothing to resolve it.

	// The fixture itself is rooted for the duration, because the collection below is real: without this the
	// subsystems and the row would go with the stand-in and the case would prove nothing about the stand-in.
	FGCObjectScopeGuard KeepEntities(Entities);
	FGCObjectScopeGuard KeepModel(Model);
	FGCObjectScopeGuard KeepRow(RowStandIn);

	const FGuid AnchorNetID(0xA3, 1, 2, 3);
	Entities->RegisterEntity(MakeDerivedContainerRecord(AnchorNetID, RowStandIn, ActorClassID));

	if (!TestEqual(TEXT("the row stood in for its class's component container"),
		Model->EnrollDerivedComponentContainers(AnchorNetID, RowStandIn), 1))
	{
		return false;
	}

	TArray<FGuid> Subs;
	Model->GetSubParticipants(AnchorNetID, Subs);
	if (!TestEqual(TEXT("which is tracked under the anchor"), Subs.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("and the subsystem holds it strongly, which is the only thing that can"),
		Model->GetHeldContainerStandInCountForTest(), 1);

	const FCrowdyEntityRecord* Before = Entities->FindRecord(Subs[0]);
	if (!TestNotNull(TEXT("the stand-in enrolled a record"), Before)
		|| !TestNotNull(TEXT("naming a live participant"), Before->GetParticipant()))
	{
		return false;
	}

	// The whole point. A purging collection is what happens roughly once a minute in a running game.
	CollectGarbage(RF_NoFlags, /*bPerformFullPurge*/ true);

	const FCrowdyEntityRecord* After = Entities->FindRecord(Subs[0]);
	if (!TestNotNull(TEXT("the record is still there after a collection"), After))
	{
		return false;
	}
	TestNotNull(TEXT("and it still resolves to a live stand-in, which is what applies every later pull"),
		After->GetParticipant());
	TestTrue(TEXT("resolving the participant is what the pull completion does, so ask exactly that"),
		::IsValid(After->GetParticipant()));

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

/**
 * The number of containers one entity may be stood in for is capped, because the class it records is chosen by
 * that entity's own owner.
 *
 * Each stand-in that finds no server row stays pending and is re-driven forever against a shared call allowance,
 * so an entity naming a class with many CrowdyContainer components multiplies what one row costs an observer.
 * Mutating away the Derived.SetNum in EnrollDerivedComponentContainers turns this red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyDerivedContainerCountIsCappedTest,
	"CrowdySDK.GameModel.DerivedContainerCountIsCapped", CrowdyDerivedContainerTestFlags)
bool FCrowdyDerivedContainerCountIsCappedTest::RunTest(const FString& Parameters)
{
	uint32 ActorClassID = 0;
	if (!TryRecordDerivedContainerClass(ACrowdyEntityClassManyContainersActor::StaticClass(), ActorClassID))
	{
		AddInfo(TEXT("the class registry is sealed in this process, so the id could not be made resolvable; skipping."));
		return true;
	}

	TArray<FCrowdyDerivedComponentContainer> Derived;
	CrowdyEntityClassContainer::GetDerivedComponentContainers(
		ACrowdyEntityClassManyContainersActor::StaticClass(), Derived);
	if (!TestTrue(TEXT("the fixture really declares more containers than an entity may spend on"),
		Derived.Num() > UCrowdyGameModelSubsystem::MaxDerivedContainersPerEntity))
	{
		return false;
	}

	UCrowdyEntitySubsystem* Entities = MakeDerivedContainerEntities();
	UCrowdyGameModelSubsystem* Model = MakeDerivedContainerModel(Entities);
	AddExpectedErrorPlain(TEXT("only the first"), EAutomationExpectedErrorFlags::Contains, 0);

	const FGuid AnchorNetID(0xA4, 1, 2, 3);
	UObject* RowStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	Entities->RegisterEntity(MakeDerivedContainerRecord(AnchorNetID, RowStandIn, ActorClassID));

	TestEqual(TEXT("the row is stood in for the capped number of containers, not the declared number"),
		Model->EnrollDerivedComponentContainers(AnchorNetID, RowStandIn),
		UCrowdyGameModelSubsystem::MaxDerivedContainersPerEntity);

	TArray<FGuid> Subs;
	Model->GetSubParticipants(AnchorNetID, Subs);
	TestEqual(TEXT("and that is what the registry actually holds for it"),
		Subs.Num(), UCrowdyGameModelSubsystem::MaxDerivedContainersPerEntity);

	return true;
}

/**
 * The coalesce window a refresh pull waits widens as the call allowance is spent.
 *
 * A pull IS a Game API call against the same per-window allowance an invoke spends, and it is counted in the same
 * ledger. A window fixed at its authored length emits pulls at one rate into an allowance those pulls are
 * themselves spending, so a fight surfaces as the server refusing rather than as this client merging harder.
 * Mutating ResolveRefreshPullWindowSeconds back to returning RefreshPullCoalesceSeconds turns this red.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRefreshPullWindowWidensUnderBudgetTest,
	"CrowdySDK.GameModel.RefreshPullWindowWidensUnderBudget", CrowdyDerivedContainerTestFlags)
bool FCrowdyRefreshPullWindowWidensUnderBudgetTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeDerivedContainerEntities();
	UCrowdyGameModelSubsystem* Model = MakeDerivedContainerModel(Entities);

	TestEqual(TEXT("with the allowance untouched a pull waits exactly as long as it is authored to"),
		Model->GetRefreshPullWindowSecondsForTest(),
		UCrowdyGameModelSubsystem::RefreshPullCoalesceSeconds);

	// A fight's worth of calls, whatever made them: the allowance does not care which kind they were.
	const double Now = FApp::GetCurrentTime();
	for (int32 Index = 0; Index < UCrowdyGameModelSubsystem::InvokeBudgetLimitPerWindow; ++Index)
	{
		Model->RecordInvokeAttemptForTest(Now);
	}

	TestTrue(TEXT("with the allowance spent the same pull waits longer, so fewer of them are sent"),
		Model->GetRefreshPullWindowSecondsForTest() > UCrowdyGameModelSubsystem::RefreshPullCoalesceSeconds);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
