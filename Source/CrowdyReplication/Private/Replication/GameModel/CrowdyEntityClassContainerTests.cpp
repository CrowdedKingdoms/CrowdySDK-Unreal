#include "Replication/GameModel/CrowdyEntityClassContainer.h"

// WITH_METADATA as well as the test flag: the fixtures here declare their container through UCLASS metadata and
// carry CrowdyContainerTest, which the registry baker excludes on purpose, so outside the editor there is no source
// for their tag at all and every case that reads one would report a working walk as broken.
#if WITH_DEV_AUTOMATION_TESTS && WITH_METADATA

#include "Core/FCrowdyTypeID.h"
#include "Data/CrowdyEntityTypes.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership
#include "Replication/GameModel/CrowdyEntityClassContainerTestTypes.h"
#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "Utils/UCrowdyClassRegistry.h"

// A remote player drawn as a crowd row is represented on an observer's machine by a stand-in object rather than by
// the actor its owner runs. The stand-in declares nothing, so before this the entity enrolled, bound no container,
// and every effect aimed at it was refused as unbound. These cover the fold that fixes it: the container comes from
// the class the ENTITY records (the same actor class its owner runs, declared once), the binding it produces is
// read-only whatever the entity's role says, and an entity whose recorded class declares nothing is still left
// alone.
namespace
{
	constexpr EAutomationTestFlags CrowdyEntityClassTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Distinct helper names so this test TU never collides with the other Game Model test helpers in a unity build.
	UCrowdyEntitySubsystem* MakeEntityClassEntities(const FGuid& LocalPlayer)
	{
		UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
		Entities->SetLocalPlayerID(LocalPlayer);
		return Entities;
	}

	UCrowdyGameModelSubsystem* MakeEntityClassModel(UCrowdyEntitySubsystem* Entities)
	{
		UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
		Model->SetEntitySubsystemForTest(Entities);
		return Model;
	}

	// Puts a class in the class registry the way the startup scan does, and reports whether the round trip an entity
	// record depends on actually works: a record carries only a ClassID, so the class is unreachable without it. The
	// registry seals once a game instance starts, so a headless run that follows a PIE session in the same process
	// answers false here rather than failing somewhere less obvious.
	bool TryRecordEntityClass(const UClass* Class, uint32& OutClassID)
	{
		UCrowdyClassRegistry* Registry = UCrowdyClassRegistry::Get();
		const FCrowdyClassID ClassID = Registry->GetID(Class);
		Registry->RegisterClass(ClassID, FSoftClassPath(Class));
		OutClassID = ClassID;
		return Registry->Resolve(ClassID).ResolveClass() == Class;
	}

	// A record shaped like the one a crowd entity's enrollment writes: an id, the class the entity is recorded as,
	// and a stand-in participant that is not the entity's actor.
	FCrowdyEntityRecord MakeStandInRecord(const FGuid& NetID, UObject* StandIn, ECrowdyRole Role, uint32 ClassID)
	{
		FCrowdyEntityRecord Record;
		Record.NetID = NetID;
		Record.OwnerID = FGuid::NewGuid(); // another client simulates it
		Record.Role = Role;
		Record.ClassID = ClassID;
		Record.Participant = StandIn;
		return Record;
	}

	// An id the class registry has never been told about, confirmed unknown rather than assumed so. Stands in for
	// an entity drawn as a class this client never loaded.
	uint32 FindUnknownClassID()
	{
		for (uint32 Candidate = 0x0C0FFEE1; Candidate < 0x0C0FFEF0; ++Candidate)
		{
			if (!UCrowdyClassRegistry::Get()->Resolve(Candidate).IsValid())
			{
				return Candidate;
			}
		}
		return CROWDY_INVALID_CLASS_ID;
	}

	// Both branches of the Game API context check name the Game API, and a headless subsystem has no world, so a
	// resolve that is actually attempted always logs one of them. Whitelisting the pair is what separates "the bind
	// was refused before it began" from "it resolved a container and went looking for the row".
	void ExpectGameApiContextFailure(FAutomationTestBase& Test)
	{
		Test.AddExpectedErrorPlain(TEXT("Game API"), EAutomationExpectedErrorFlags::Contains, 0);
	}
}

// The enrollment path itself: a crowd entity registers with a stand-in participant, and the container it binds is
// the one its recorded actor class declares. Before this the same registration resolved no type and returned, which
// is the Unbound refusal every effect aimed at a crowd entity hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdEntityBindsFromRecordedClassTest,
	"CrowdySDK.GameModel.CrowdEntityBindsFromRecordedClass", CrowdyEntityClassTestFlags)
bool FCrowdyCrowdEntityBindsFromRecordedClassTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntityClassEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = MakeEntityClassModel(Entities);
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}

	uint32 ClassID = 0;
	if (!TestTrue(TEXT("the recorded actor class round-trips through the class registry"),
		TryRecordEntityClass(ACrowdyEntityClassContainerActor::StaticClass(), ClassID)))
	{
		return false;
	}

	UObject* StandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	const FGuid NetID = FGuid::NewGuid();
	Entities->RegisterEntity(MakeStandInRecord(NetID, StandIn, ECrowdyRole::RemoteProxy, ClassID));

	// The stand-in is what the bind sees, and it declares nothing.
	FString StandInType;
	TestFalse(TEXT("the stand-in really declares no container of its own"),
		CrowdyEntityClassContainer::TryGetContainerTypeName(StandIn->GetClass(), StandInType));

	ExpectGameApiContextFailure(*this);
	Model->HandleEntityRegisteredForTest(NetID);

	FString PendingType;
	TestTrue(TEXT("the crowd entity resolved a container instead of being refused"),
		Model->TryGetPendingModelEntityTypeForTest(NetID, PendingType));
	TestEqual(TEXT("it is the type the recorded actor class declares"), PendingType, TEXT("TestCrowdActor"));
	TestTrue(TEXT("the binding is recorded as class-derived"), Model->IsClassDerivedBindingForTest(NetID));

	return true;
}

// A class-derived binding may only READ. The recorded class is chosen by the entity's own owner, so a record that
// would otherwise be authoritative to create (a Host-owned one) must still not ensure a row; a participant that
// declares its own container is unaffected and stays authoritative.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdEntityBindingIsReadOnlyTest,
	"CrowdySDK.GameModel.CrowdEntityBindingIsReadOnly", CrowdyEntityClassTestFlags)
bool FCrowdyCrowdEntityBindingIsReadOnlyTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntityClassEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = MakeEntityClassModel(Entities);
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}

	uint32 ClassID = 0;
	if (!TestTrue(TEXT("the recorded actor class round-trips through the class registry"),
		TryRecordEntityClass(ACrowdyEntityClassContainerActor::StaticClass(), ClassID)))
	{
		return false;
	}

	UObject* StandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	const FGuid DerivedNetID = FGuid::NewGuid();
	// Host-owned is the role that IS authoritative to create, so this is the record the clamp has to hold on.
	Entities->RegisterEntity(MakeStandInRecord(DerivedNetID, StandIn, ECrowdyRole::HostOwned, ClassID));

	ExpectGameApiContextFailure(*this);
	Model->HandleEntityRegisteredForTest(DerivedNetID);

	TestTrue(TEXT("the class-derived binding was taken"), Model->IsClassDerivedBindingForTest(DerivedNetID));
	TestFalse(TEXT("a class-derived binding may not create a container row"),
		Model->IsAuthoritativeToCreateForTest(DerivedNetID));

	// Control: the same Host-owned role, but the participant's own class declares the container, so nothing about
	// this binding came from a recorded class and it keeps the authority it has always had.
	AActor* OwnActor = NewObject<ACrowdyEntityClassContainerActor>(GetTransientPackage());
	const FGuid OwnNetID = Entities->RegisterParticipant(OwnActor, ECrowdyOwnership::Host);
	if (!TestTrue(TEXT("the container actor registered"), OwnNetID.IsValid()))
	{
		return false;
	}
	Model->HandleEntityRegisteredForTest(OwnNetID);

	TestFalse(TEXT("a participant that declares its own container is not a class-derived binding"),
		Model->IsClassDerivedBindingForTest(OwnNetID));
	TestTrue(TEXT("and it is still authoritative to create its row"),
		Model->IsAuthoritativeToCreateForTest(OwnNetID));

	return true;
}

// The participant's own declaration outranks the recorded class: an actor that declares a container binds ITS type,
// never the one the record happens to name. Without this precedence a record could redirect a real actor's own
// container to another type.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyParticipantContainerOutranksRecordedClassTest,
	"CrowdySDK.GameModel.ParticipantContainerOutranksRecordedClass", CrowdyEntityClassTestFlags)
bool FCrowdyParticipantContainerOutranksRecordedClassTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntityClassEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = MakeEntityClassModel(Entities);
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}

	uint32 OtherClassID = 0;
	if (!TestTrue(TEXT("the other container class round-trips through the class registry"),
		TryRecordEntityClass(ACrowdyEntityClassContainerNoPullActor::StaticClass(), OtherClassID)))
	{
		return false;
	}

	// An actor that declares "TestCrowdActor", recorded as the class that declares "TestCrowdActorNoPull".
	AActor* OwnActor = NewObject<ACrowdyEntityClassContainerActor>(GetTransientPackage());
	const FGuid NetID = FGuid::NewGuid();
	FCrowdyEntityRecord Record = MakeStandInRecord(NetID, OwnActor, ECrowdyRole::RemoteProxy, OtherClassID);
	Entities->RegisterEntity(Record);

	ExpectGameApiContextFailure(*this);
	Model->HandleEntityRegisteredForTest(NetID);

	FString PendingType;
	TestTrue(TEXT("the actor bound a container"), Model->TryGetPendingModelEntityTypeForTest(NetID, PendingType));
	TestEqual(TEXT("it is the type the actor itself declares"), PendingType, TEXT("TestCrowdActor"));
	TestFalse(TEXT("and the record's class did not make it a class-derived binding"),
		Model->IsClassDerivedBindingForTest(NetID));

	return true;
}

// An entity whose recorded class declares no container is still left alone: nothing is pending, nothing is marked,
// and no resolve is attempted at all. The absence of any Game API error here is the assertion that the bind never
// began - whitelisting one would hide it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdEntityWithoutRecordedContainerTest,
	"CrowdySDK.GameModel.CrowdEntityWithoutRecordedContainerStaysUnbound", CrowdyEntityClassTestFlags)
bool FCrowdyCrowdEntityWithoutRecordedContainerTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntityClassEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = MakeEntityClassModel(Entities);
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}

	uint32 PlainClassID = 0;
	if (!TestTrue(TEXT("the plain actor class round-trips through the class registry"),
		TryRecordEntityClass(ACrowdyEntityClassPlainActor::StaticClass(), PlainClassID)))
	{
		return false;
	}

	UObject* StandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	const FGuid NetID = FGuid::NewGuid();
	Entities->RegisterEntity(MakeStandInRecord(NetID, StandIn, ECrowdyRole::RemoteProxy, PlainClassID));

	Model->HandleEntityRegisteredForTest(NetID);

	FString PendingType;
	TestFalse(TEXT("an entity whose recorded class declares no container binds nothing"),
		Model->TryGetPendingModelEntityTypeForTest(NetID, PendingType));
	TestFalse(TEXT("and it is not marked as a class-derived binding"),
		Model->IsClassDerivedBindingForTest(NetID));

	return true;
}

// The pull-on-bind setting is a property of the container type, so a class-derived binding has to read it off the
// recorded class. Asking the stand-in instead would answer "pull" for every crowd entity, whatever its container
// type says, and the retry sweep asks this question again on every attempt.
//
// Every row below is one where the participant's own class and the recorded class DISAGREE about pulling, so each
// answer names which of the two produced it. That matters because a participant declaring no container answers the
// default, which is "pull": a row expecting a pull is satisfied by the stand-in's default whether the recorded
// class is consulted or not, and cannot say anything about where the answer came from. Only a row expecting NOT to
// pull can, since nothing but a container class carrying the opt-out ever produces one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdEntityPullSettingFromRecordedClassTest,
	"CrowdySDK.GameModel.CrowdEntityPullSettingComesFromRecordedClass", CrowdyEntityClassTestFlags)
bool FCrowdyCrowdEntityPullSettingFromRecordedClassTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntityClassEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = MakeEntityClassModel(Entities);
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}

	uint32 PullOnClassID = 0;
	uint32 NoPullClassID = 0;
	if (!TestTrue(TEXT("the pulling container class round-trips"),
			TryRecordEntityClass(ACrowdyEntityClassContainerActor::StaticClass(), PullOnClassID))
		|| !TestTrue(TEXT("the opted-out container class round-trips"),
			TryRecordEntityClass(ACrowdyEntityClassContainerNoPullActor::StaticClass(), NoPullClassID)))
	{
		return false;
	}

	// The three answers every row below is built out of, stated first so a row's expectation can be traced back to
	// exactly one source. Without the third of these the disagreements are only asserted by assumption.
	TestTrue(TEXT("the first container class pulls on bind"),
		UCrowdyGameModelSubsystem::ShouldPullOnBindForClass(ACrowdyEntityClassContainerActor::StaticClass()));
	TestFalse(TEXT("the second container class opted out"),
		UCrowdyGameModelSubsystem::ShouldPullOnBindForClass(ACrowdyEntityClassContainerNoPullActor::StaticClass()));
	TestTrue(TEXT("and a stand-in, declaring no container at all, answers the default, which is to pull"),
		UCrowdyGameModelSubsystem::ShouldPullOnBindForClass(UCrowdyEntityClassStandIn::StaticClass()));

	// A stand-in participant against an opted-out recorded class. The participant's own answer is "pull" and the
	// expected answer is "do not", so this row is satisfied by the recorded class and by nothing else. It is the
	// one assertion here that fails if the recorded-class fallback stops being consulted.
	UObject* QuietStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	const FGuid QuietNetID = FGuid::NewGuid();
	Entities->RegisterEntity(MakeStandInRecord(QuietNetID, QuietStandIn, ECrowdyRole::RemoteProxy, NoPullClassID));
	TestFalse(TEXT("a crowd entity of an opted-out container type does not pull, though its stand-in would"),
		Model->ShouldPullOnRetryForEntityForTest(QuietNetID));

	// The same stand-in against a pulling recorded class. Both sources say "pull" here, so this row distinguishes
	// nothing on its own; it is kept as the control saying the refusal above was about the setting rather than
	// about a stand-in-backed entity never pulling at all.
	UObject* PullingStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	const FGuid PullingNetID = FGuid::NewGuid();
	Entities->RegisterEntity(MakeStandInRecord(PullingNetID, PullingStandIn, ECrowdyRole::RemoteProxy, PullOnClassID));
	TestTrue(TEXT("a crowd entity of a pulling container type pulls when its retry lands"),
		Model->ShouldPullOnRetryForEntityForTest(PullingNetID));

	// A participant that declares its own container binds THAT container, so the setting has to follow the binding
	// that was actually taken rather than the record. Here the two disagree the other way round: the participant
	// opted out, the class the record names pulls, and only the participant's answer is the right one.
	AActor* QuietActor = NewObject<ACrowdyEntityClassContainerNoPullActor>(GetTransientPackage());
	const FGuid QuietActorNetID = FGuid::NewGuid();
	Entities->RegisterEntity(MakeStandInRecord(QuietActorNetID, QuietActor, ECrowdyRole::RemoteProxy, PullOnClassID));
	TestFalse(TEXT("an opted-out participant does not pull just because the class its record names would"),
		Model->ShouldPullOnRetryForEntityForTest(QuietActorNetID));

	// And the mirror image, so neither direction of that precedence can be satisfied by a constant answer.
	AActor* PullingActor = NewObject<ACrowdyEntityClassContainerActor>(GetTransientPackage());
	const FGuid PullingActorNetID = FGuid::NewGuid();
	Entities->RegisterEntity(MakeStandInRecord(PullingActorNetID, PullingActor, ECrowdyRole::RemoteProxy, NoPullClassID));
	TestTrue(TEXT("a pulling participant still pulls though the class its record names opted out"),
		Model->ShouldPullOnRetryForEntityForTest(PullingActorNetID));

	// An entity that went away during the round trip never pulls, whatever any class says.
	TestFalse(TEXT("an unregistered entity does not pull"),
		Model->ShouldPullOnRetryForEntityForTest(FGuid::NewGuid()));

	return true;
}

// Resolution selects among classes already in memory and creates nothing for an id it does not know. A record only
// ever carries an id, and an id that names nothing here is an ordinary answer, not a reason to load a package.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyRecordedClassResolutionIsLoadedOnlyTest,
	"CrowdySDK.GameModel.RecordedClassResolutionIsLoadedOnly", CrowdyEntityClassTestFlags)
bool FCrowdyRecordedClassResolutionIsLoadedOnlyTest::RunTest(const FString& Parameters)
{
	TestNull(TEXT("an entity that recorded no class resolves to nothing"),
		CrowdyEntityClassContainer::ResolveRecordedClass(CROWDY_INVALID_CLASS_ID));

	const uint32 UnknownID = FindUnknownClassID();
	if (!TestTrue(TEXT("found an id the registry does not know"), UnknownID != CROWDY_INVALID_CLASS_ID))
	{
		return false;
	}
	TestNull(TEXT("an unknown class id resolves to nothing"),
		CrowdyEntityClassContainer::ResolveRecordedClass(UnknownID));

	// A native container class is its own tagged class, so normalizing one never loses its declaration.
	const UClass* Container = ACrowdyEntityClassContainerActor::StaticClass();
	TestTrue(TEXT("a native class is the class its tags are stamped on"),
		CrowdyEntityClassContainer::ResolveTaggedClass(Container) == Container);
	FString TypeName;
	TestTrue(TEXT("and its container type reads back through the normalization"),
		CrowdyEntityClassContainer::TryGetContainerTypeName(Container, TypeName));
	TestEqual(TEXT("as the declared type"), TypeName, TEXT("TestCrowdActor"));
	TestNull(TEXT("a null class normalizes to nothing"), CrowdyEntityClassContainer::ResolveTaggedClass(nullptr));

	return true;
}

// An entity drawn as a class this client never loaded cannot have its declaration read, so it binds nothing. That
// is indistinguishable from a server that has not created the rows yet, so it says so - once for the whole world,
// because every crowd entity of that class hits the same missing class.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyCrowdEntityUnloadedRecordedClassWarnsOnceTest,
	"CrowdySDK.GameModel.CrowdEntityUnloadedRecordedClassWarnsOnce", CrowdyEntityClassTestFlags)
bool FCrowdyCrowdEntityUnloadedRecordedClassWarnsOnceTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntityClassEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = MakeEntityClassModel(Entities);
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}

	const uint32 UnknownID = FindUnknownClassID();
	if (!TestTrue(TEXT("found an id the registry does not know"), UnknownID != CROWDY_INVALID_CLASS_ID))
	{
		return false;
	}

	UObject* FirstStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	UObject* SecondStandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	const FGuid FirstNetID = FGuid::NewGuid();
	const FGuid SecondNetID = FGuid::NewGuid();
	Entities->RegisterEntity(MakeStandInRecord(FirstNetID, FirstStandIn, ECrowdyRole::RemoteProxy, UnknownID));
	Entities->RegisterEntity(MakeStandInRecord(SecondNetID, SecondStandIn, ECrowdyRole::RemoteProxy, UnknownID));

	// Exactly one: a second occurrence fails this, which is the whole claim.
	AddExpectedErrorPlain(TEXT("is not loaded on this client"), EAutomationExpectedErrorFlags::Contains, 1);
	Model->HandleEntityRegisteredForTest(FirstNetID);
	Model->HandleEntityRegisteredForTest(SecondNetID);

	FString PendingType;
	TestFalse(TEXT("neither entity bound a container"),
		Model->TryGetPendingModelEntityTypeForTest(FirstNetID, PendingType));
	TestFalse(TEXT("nor the second"), Model->TryGetPendingModelEntityTypeForTest(SecondNetID, PendingType));

	return true;
}

// The read-only mark belongs to the entity, not to the id: unregistration drops it, so an id reused by a later
// participant that declares its own container is not silently held read-only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyClassDerivedBindingClearsOnUnregisterTest,
	"CrowdySDK.GameModel.ClassDerivedBindingClearsOnUnregister", CrowdyEntityClassTestFlags)
bool FCrowdyClassDerivedBindingClearsOnUnregisterTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeEntityClassEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = MakeEntityClassModel(Entities);
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}

	uint32 ClassID = 0;
	if (!TestTrue(TEXT("the recorded actor class round-trips through the class registry"),
		TryRecordEntityClass(ACrowdyEntityClassContainerActor::StaticClass(), ClassID)))
	{
		return false;
	}

	UObject* StandIn = NewObject<UCrowdyEntityClassStandIn>(GetTransientPackage());
	const FGuid NetID = FGuid::NewGuid();
	Entities->RegisterEntity(MakeStandInRecord(NetID, StandIn, ECrowdyRole::HostOwned, ClassID));

	ExpectGameApiContextFailure(*this);
	Model->HandleEntityRegisteredForTest(NetID);
	TestTrue(TEXT("the class-derived binding was taken"), Model->IsClassDerivedBindingForTest(NetID));

	Model->HandleEntityUnregisteredForTest(NetID);
	TestFalse(TEXT("unregistration drops the read-only mark"), Model->IsClassDerivedBindingForTest(NetID));

	// The same id, now held by a Host-owned participant that declares its own container, is authoritative again.
	Entities->UnregisterEntity(NetID);
	AActor* OwnActor = NewObject<ACrowdyEntityClassContainerActor>(GetTransientPackage());
	FCrowdyEntityRecord Reused;
	Reused.NetID = NetID;
	Reused.Role = ECrowdyRole::HostOwned;
	Reused.Participant = OwnActor;
	Entities->RegisterEntity(Reused);
	TestTrue(TEXT("the reused id is authoritative to create again"), Model->IsAuthoritativeToCreateForTest(NetID));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_METADATA
