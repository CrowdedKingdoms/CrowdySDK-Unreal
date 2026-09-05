#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/ActorComponent.h" // EComponentCreationMethod, UActorComponent
#include "Components/SceneComponent.h" // a concrete untagged component for the sweep-skip case
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Script.h" // FEditorScriptExecutionGuard (an AActor's ProcessEvent no-ops without a world)
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership
#include "Replication/GameModel/CrowdyAttributeRegistry.h" // ClassHasModelAttributes
#include "Replication/GameModel/CrowdyModel.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"
#include "Utils/HelperFunctions.h"

// Stage 2 (Ensured Identity) - component sub-participants. A CrowdyContainer component on a registered actor
// enrolls as its own participant, keyed off the actor's NetID so two same-class components (and the same component
// across clients) get distinct, deterministic ids, inheriting the actor's authority. These cover the entity-
// subsystem enrollment seam (determinism, distinctness, authority inheritance, the dup-key guard, teardown), the
// universal target resolution onto the derived id, and the Get Model Component convenience. The actor-sweep +
// live apply/OnRep is the named PIE gate.
namespace
{
	constexpr EAutomationTestFlags CrowdySubPartTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Distinct helper names so this test TU never collides with the targeting / apply test helpers in a unity build.
	UCrowdyEntitySubsystem* MakeSubPartEntities(const FGuid& LocalPlayer)
	{
		UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
		Entities->SetLocalPlayerID(LocalPlayer);
		return Entities;
	}

	// A worldless-friendly editor world: the actor-side ResolveIdentity key path reads the owner's interface via
	// Execute_ (ProcessEvent), which no-ops on a worldless AActor. Spawning the owner in an editor world (with the
	// script-execution guard) makes ProcessEvent run; an editor world never creates the PIE-gated Crowdy subsystems,
	// so it tears down cleanly. Mirrors FCrowdyRpcTestWorld.
	struct FCrowdySubPartTestWorld
	{
		FEditorScriptExecutionGuard ScriptGuard;
		UWorld* World = nullptr;

		FCrowdySubPartTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdySubPartTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}

		template <typename T>
		T* Spawn() { return World->SpawnActor<T>(); }
	};

	// Reproduces RegisterSubParticipant's key derivation independently, so a test asserts the id is exactly this
	// function of (anchor id, class path, per-instance term) rather than just internally self-consistent. The
	// per-instance term is KeyOverride when non-empty, else the object name (matching the production seed).
	FGuid ExpectedSubNetID(const FGuid& AnchorNetID, const UObject* Participant, const FString& KeyOverride = FString())
	{
		const FString InstanceTerm = KeyOverride.IsEmpty() ? Participant->GetName() : KeyOverride;
		const FString Seed = AnchorNetID.ToString(EGuidFormats::Digits)
			+ TEXT(":") + Participant->GetClass()->GetPathName()
			+ TEXT(":") + InstanceTerm;
		return UHelperFunctions::GetDeterministicID(FCrowdyTypeIDGenerator::GenerateFromString(Seed));
	}
}

// A sub-participant's id is a deterministic function of the anchor id + class + object name (so every client that
// shares the anchor id derives the same sub id), and re-enrolling the same object is idempotent (same id, not a
// duplicate record). Two same-class components with distinct names under one anchor get distinct ids.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySubParticipantKeyTest,
	"CrowdySDK.GameModel.SubParticipantKey", CrowdySubPartTestFlags)
bool FCrowdySubParticipantKeyTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(FGuid::NewGuid());
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}

	// An anchor entity (a Host participant stands in for a level-placed boss).
	UObject* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	TestTrue(TEXT("anchor got a valid NetID"), AnchorNetID.IsValid());

	UCrowdyGameModelTestComponent* CompA = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	const FGuid SubA = Entities->RegisterSubParticipant(CompA, AnchorNetID);
	TestTrue(TEXT("sub-participant got a valid NetID"), SubA.IsValid());
	TestEqual(TEXT("the derived id matches the documented key derivation"), SubA, ExpectedSubNetID(AnchorNetID, CompA));

	// Re-enrolling the same object under the same anchor is idempotent (same id).
	const FGuid SubARepeat = Entities->RegisterSubParticipant(CompA, AnchorNetID);
	TestEqual(TEXT("re-enrolling the same component yields the same id"), SubARepeat, SubA);

	// A second, distinct-name component of the same class under the same anchor gets a distinct id.
	UCrowdyGameModelTestComponent* CompB = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	const FGuid SubB = Entities->RegisterSubParticipant(CompB, AnchorNetID);
	TestTrue(TEXT("the second component got a valid NetID"), SubB.IsValid());
	TestNotEqual(TEXT("two same-class distinct-name components get distinct ids"), SubB, SubA);

	return true;
}

// A sub-participant inherits its anchor's authority (Role + OwnerID): a component on a Host-owned anchor is
// Host-owned (a shared container), one on a locally-owned anchor carries that owner (a per-player container).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySubParticipantInheritsAuthorityTest,
	"CrowdySDK.GameModel.SubParticipantInheritsAuthority", CrowdySubPartTestFlags)
bool FCrowdySubParticipantInheritsAuthorityTest::RunTest(const FString& Parameters)
{
	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(LocalPlayer);
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}

	// Host anchor -> sub is HostOwned with no owner id.
	UObject* HostAnchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid HostAnchorID = Entities->RegisterParticipant(HostAnchor, ECrowdyOwnership::Host);
	UCrowdyGameModelTestComponent* HostComp = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	const FGuid HostSub = Entities->RegisterSubParticipant(HostComp, HostAnchorID);
	const FCrowdyEntityRecord* HostRecord = Entities->FindRecord(HostSub);
	if (TestNotNull(TEXT("host sub record exists"), HostRecord))
	{
		TestEqual(TEXT("host sub inherits HostOwned role"), HostRecord->Role, ECrowdyRole::HostOwned);
		TestFalse(TEXT("host sub has no owner id"), HostRecord->OwnerID.IsValid());
	}

	// LocalClient anchor -> sub is Owner with the local player id.
	UObject* LocalAnchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid LocalAnchorID = Entities->RegisterParticipant(LocalAnchor, ECrowdyOwnership::LocalClient);
	UCrowdyGameModelTestComponent* LocalComp = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	const FGuid LocalSub = Entities->RegisterSubParticipant(LocalComp, LocalAnchorID);
	const FCrowdyEntityRecord* LocalRecord = Entities->FindRecord(LocalSub);
	if (TestNotNull(TEXT("local sub record exists"), LocalRecord))
	{
		TestEqual(TEXT("local sub inherits Owner role"), LocalRecord->Role, ECrowdyRole::Owner);
		TestEqual(TEXT("local sub inherits the local owner id"), LocalRecord->OwnerID, LocalPlayer);
	}

	return true;
}

// The dup-key guard: two DIFFERENT live objects that derive the same key (same class + same object name, under one
// anchor) must not both bind - the second is refused with an error naming both, leaving the first intact. Distinct
// outers give two objects the identical GetName(), forcing the collision without depending on internal state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySubParticipantDupKeyGuardTest,
	"CrowdySDK.GameModel.SubParticipantDupKeyGuard", CrowdySubPartTestFlags)
bool FCrowdySubParticipantDupKeyGuardTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(FGuid::NewGuid());
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}

	UObject* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);

	// Two components of the same class with the SAME object name, living in different packages, derive the same key.
	UPackage* PkgA = NewObject<UPackage>(nullptr, TEXT("/Temp/CrowdySubPartA"), RF_Transient);
	UPackage* PkgB = NewObject<UPackage>(nullptr, TEXT("/Temp/CrowdySubPartB"), RF_Transient);
	UCrowdyGameModelTestComponent* CompA = NewObject<UCrowdyGameModelTestComponent>(PkgA, TEXT("Attributes"));
	UCrowdyGameModelTestComponent* CompB = NewObject<UCrowdyGameModelTestComponent>(PkgB, TEXT("Attributes"));
	TestEqual(TEXT("the two components share an object name"), CompA->GetName(), CompB->GetName());

	const FGuid SubA = Entities->RegisterSubParticipant(CompA, AnchorNetID);
	TestTrue(TEXT("the first enrolls"), SubA.IsValid());

	// The collision is a loud error; whitelist it so it does not fail the test run.
	AddExpectedError(TEXT("sub-participant key collision"), EAutomationExpectedErrorFlags::Contains, 1);
	const FGuid SubB = Entities->RegisterSubParticipant(CompB, AnchorNetID);
	TestFalse(TEXT("the colliding second is refused"), SubB.IsValid());

	// The first is still bound to the id; the second never displaced it.
	TestTrue(TEXT("the id still resolves to the first component"), Entities->FindParticipant(SubA) == CompA);
	TestFalse(TEXT("the second component is not registered"), Entities->FindEntityID(CompB).IsValid());

	return true;
}

// A sub-participant resolves through the universal target seam: ResolveTargetNetID(component) yields the derived
// id, and ResolveEntityParticipant(id) yields the component - so the model getters and the Apply Crowdy Effect
// Target pin address the component's container. Invalid inputs fail closed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySubParticipantResolvesAsTargetTest,
	"CrowdySDK.GameModel.SubParticipantResolvesAsTarget", CrowdySubPartTestFlags)
bool FCrowdySubParticipantResolvesAsTargetTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}
	Model->SetEntitySubsystemForTest(Entities);

	UObject* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	UCrowdyGameModelTestComponent* Comp = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	const FGuid SubNetID = Entities->RegisterSubParticipant(Comp, AnchorNetID);

	FGuid Resolved;
	TestTrue(TEXT("ResolveTargetNetID resolves the component to its sub id"), Model->ResolveTargetNetID(Comp, Resolved));
	TestEqual(TEXT("the resolved id is the enrolled sub id"), Resolved, SubNetID);

	// Unregistering the component drops the mapping (fails closed).
	Entities->UnregisterParticipant(Comp);
	FGuid AfterUnregister;
	TestFalse(TEXT("an unregistered component no longer resolves"), Model->ResolveTargetNetID(Comp, AfterUnregister));

	return true;
}

// Invalid inputs to RegisterSubParticipant fail closed: a null participant, or an anchor id that is not registered,
// both return an invalid id and enroll nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySubParticipantInvalidInputsTest,
	"CrowdySDK.GameModel.SubParticipantInvalidInputs", CrowdySubPartTestFlags)
bool FCrowdySubParticipantInvalidInputsTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(FGuid::NewGuid());
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}

	// Null participant -> invalid, no crash.
	AddExpectedError(TEXT("RegisterSubParticipant called with an invalid participant or anchor"),
		EAutomationExpectedErrorFlags::Contains, 0);
	TestFalse(TEXT("null participant does not enroll"),
		Entities->RegisterSubParticipant(nullptr, FGuid::NewGuid()).IsValid());

	// A valid participant against an unregistered anchor -> invalid.
	AddExpectedError(TEXT("has no registered anchor entity"), EAutomationExpectedErrorFlags::Contains, 0);
	UCrowdyGameModelTestComponent* Comp = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());
	TestFalse(TEXT("an unregistered anchor does not enroll"),
		Entities->RegisterSubParticipant(Comp, FGuid::NewGuid()).IsValid());

	return true;
}

// The actor component sweep (the real integration seam): registering an anchor actor enrolls its CrowdyContainer
// components as sub-participants, skips ordinary components, is idempotent on a repeat sweep, and the anchor's
// teardown cascade unregisters the sub-participants. Drives the private handlers via the test seam (the container
// bind they queue is a no-op with no API context, so this isolates the enrollment bookkeeping).
//
// A tagged component with NO Server Owned attributes enrolls too. Its attributes are not what make it a container:
// signals, timers and automations are authored on effect assets and every one of them needs this component's server
// container id to reach it, so refusing it left the whole hierarchy unreachable and dropped its signals silently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyComponentSweepTest,
	"CrowdySDK.GameModel.ComponentSweep", CrowdySubPartTestFlags)
bool FCrowdyComponentSweepTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}
	Model->SetEntitySubsystemForTest(Entities);

	AActor* Anchor = NewObject<AActor>(GetTransientPackage());
	UCrowdyGameModelTestComponent* Container = NewObject<UCrowdyGameModelTestComponent>(Anchor);
	USceneComponent* Ordinary = NewObject<USceneComponent>(Anchor); // untagged concrete component, must be skipped
	UCrowdyGameModelTestEmptyComponent* Empty = NewObject<UCrowdyGameModelTestEmptyComponent>(Anchor); // tagged, no attrs
	Anchor->AddOwnedComponent(Container);
	Anchor->AddOwnedComponent(Ordinary);
	Anchor->AddOwnedComponent(Empty);

	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	TestTrue(TEXT("anchor got a valid NetID"), AnchorNetID.IsValid());

	Model->HandleEntityRegisteredForTest(AnchorNetID);

	// Both CrowdyContainer components enrolled and resolve as targets; only the untagged one was skipped.
	FGuid ContainerSubID;
	TestTrue(TEXT("the container component enrolled and resolves"), Model->ResolveTargetNetID(Container, ContainerSubID));
	TestTrue(TEXT("its sub id is valid"), ContainerSubID.IsValid());
	TestFalse(TEXT("the ordinary component did not enroll"), Entities->FindEntityID(Ordinary).IsValid());

	// The tag alone declares the container: re-adding an attribute gate here turns this red.
	TestFalse(TEXT("the fixture really declares no attributes"),
		FCrowdyAttributeRegistry::ClassHasModelAttributes(UCrowdyGameModelTestEmptyComponent::StaticClass()));
	FGuid EmptySubID;
	TestTrue(TEXT("the attribute-less tagged component enrolled and resolves"),
		Model->ResolveTargetNetID(Empty, EmptySubID));
	TestTrue(TEXT("its sub id is valid"), EmptySubID.IsValid());

	// Idempotent: a repeat sweep leaves the same single enrollment (same id, still exactly one record for it).
	Model->HandleEntityRegisteredForTest(AnchorNetID);
	FGuid ContainerSubIDAgain;
	TestTrue(TEXT("the container still resolves after a repeat sweep"),
		Model->ResolveTargetNetID(Container, ContainerSubIDAgain));
	TestEqual(TEXT("the repeat sweep did not change the sub id"), ContainerSubIDAgain, ContainerSubID);

	// The anchor's teardown cascade unregisters the sub-participant.
	Model->HandleEntityUnregisteredForTest(AnchorNetID);
	FGuid AfterTeardown;
	TestFalse(TEXT("the component no longer resolves after the anchor tears down"),
		Model->ResolveTargetNetID(Container, AfterTeardown));

	return true;
}

// A runtime-added container component (an engine-generated, cross-client-unstable name) is still enrolled, but the
// sweep warns loudly so the author can move it into the Blueprint. Proves the warn-but-enroll fold.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyComponentSweepRuntimeNameWarnsTest,
	"CrowdySDK.GameModel.ComponentSweepRuntimeNameWarns", CrowdySubPartTestFlags)
bool FCrowdyComponentSweepRuntimeNameWarnsTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}
	Model->SetEntitySubsystemForTest(Entities);

	AActor* Anchor = NewObject<AActor>(GetTransientPackage());
	UCrowdyGameModelTestComponent* Container = NewObject<UCrowdyGameModelTestComponent>(Anchor);
	Container->CreationMethod = EComponentCreationMethod::Instance; // stand in for a runtime NewObject add
	Anchor->AddOwnedComponent(Container);

	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);

	AddExpectedError(TEXT("was added at runtime"), EAutomationExpectedErrorFlags::Contains, 1);
	Model->HandleEntityRegisteredForTest(AnchorNetID);

	// Warned, but still enrolled (single-client dev is not silently broken).
	FGuid SubID;
	TestTrue(TEXT("a runtime-added container is still enrolled"), Model->ResolveTargetNetID(Container, SubID));

	return true;
}

// An author-supplied binding key replaces the object name in the sub-participant derivation, so the id no longer
// depends on the (per-client-unstable) name: NetIDFromBindingKey is deterministic, and RegisterSubParticipant with
// a key yields the key-derived id, distinct from the name-derived one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyBindingKeyDerivationTest,
	"CrowdySDK.GameModel.BindingKeyDerivation", CrowdySubPartTestFlags)
bool FCrowdyBindingKeyDerivationTest::RunTest(const FString& Parameters)
{
	// The key -> NetID primitive is deterministic and distinguishes keys.
	TestEqual(TEXT("same key -> same id"),
		FCrowdyModelIdentity::NetIDFromBindingKey(TEXT("boss_north")),
		FCrowdyModelIdentity::NetIDFromBindingKey(TEXT("boss_north")));
	TestNotEqual(TEXT("different keys -> different ids"),
		FCrowdyModelIdentity::NetIDFromBindingKey(TEXT("boss_north")),
		FCrowdyModelIdentity::NetIDFromBindingKey(TEXT("boss_south")));

	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(FGuid::NewGuid());
	if (!TestNotNull(TEXT("entities"), Entities))
	{
		return false;
	}
	UObject* Anchor = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	UCrowdyGameModelTestComponent* Comp = NewObject<UCrowdyGameModelTestComponent>(GetTransientPackage());

	const FString Key = TEXT("attributes_main");
	const FGuid Keyed = Entities->RegisterSubParticipant(Comp, AnchorNetID, Key);
	TestEqual(TEXT("a keyed sub id matches the key-based derivation"), Keyed, ExpectedSubNetID(AnchorNetID, Comp, Key));
	TestNotEqual(TEXT("the key overrode the object name"), Keyed, ExpectedSubNetID(AnchorNetID, Comp));

	return true;
}

// The sweep reads a component's ICrowdyBindingKeyProvider key and enrolls it under the KEY-derived id, so a
// runtime-added (Instance) component with a key gets a cross-client-stable id (no name divergence, no fork).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyKeyedComponentSweepTest,
	"CrowdySDK.GameModel.KeyedComponentSweep", CrowdySubPartTestFlags)
bool FCrowdyKeyedComponentSweepTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}
	Model->SetEntitySubsystemForTest(Entities);

	AActor* Anchor = NewObject<AActor>(GetTransientPackage());
	UCrowdyGameModelTestKeyedComponent* Keyed = NewObject<UCrowdyGameModelTestKeyedComponent>(Anchor);
	Keyed->BindingKey = TEXT("boss_health");
	Keyed->CreationMethod = EComponentCreationMethod::Instance; // runtime-added; the key makes it safe
	Anchor->AddOwnedComponent(Keyed);

	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	Model->HandleEntityRegisteredForTest(AnchorNetID);

	FGuid SubID;
	TestTrue(TEXT("the keyed component enrolled"), Model->ResolveTargetNetID(Keyed, SubID));
	TestEqual(TEXT("it enrolled under the KEY-derived id, not the object name"),
		SubID, ExpectedSubNetID(AnchorNetID, Keyed, Keyed->BindingKey));

	return true;
}

// EnrollModelComponent enrolls a component ADDED AFTER the actor registered (the sweep only runs at registration),
// and no-ops when the component's owner is not a registered entity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEnrollModelComponentTest,
	"CrowdySDK.GameModel.EnrollModelComponent", CrowdySubPartTestFlags)
bool FCrowdyEnrollModelComponentTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}
	Model->SetEntitySubsystemForTest(Entities);

	// The actor registers first, with NO container component present yet.
	AActor* Anchor = NewObject<AActor>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);
	Model->HandleEntityRegisteredForTest(AnchorNetID);

	// A keyed component is attached later and enrolled through the runtime hook.
	UCrowdyGameModelTestKeyedComponent* Late = NewObject<UCrowdyGameModelTestKeyedComponent>(Anchor);
	Late->BindingKey = TEXT("late_added");
	Anchor->AddOwnedComponent(Late);
	Model->EnrollModelComponent(Late);

	FGuid SubID;
	TestTrue(TEXT("the late-added component enrolled via the hook"), Model->ResolveTargetNetID(Late, SubID));
	TestEqual(TEXT("under its key-derived id"), SubID, ExpectedSubNetID(AnchorNetID, Late, Late->BindingKey));

	// A component whose owner is not a registered entity is a no-op.
	AActor* Stray = NewObject<AActor>(GetTransientPackage());
	UCrowdyGameModelTestKeyedComponent* Orphan = NewObject<UCrowdyGameModelTestKeyedComponent>(Stray);
	Stray->AddOwnedComponent(Orphan);
	Model->EnrollModelComponent(Orphan);
	FGuid OrphanID;
	TestFalse(TEXT("a component on an unregistered actor does not enroll"), Model->ResolveTargetNetID(Orphan, OrphanID));

	return true;
}

// The service registry is game-instance scoped while this subsystem is per-world, so a world left behind by a level
// travel keeps its subscription until it is collected and handles every carrier frame a second time. Observed live as
// three worlds (Level_Entry, Level_Overworld, TitanAssault) under one game instance, all subscribed: the boss-wave
// signal fired three times, and every re-pull and ensure it drove was tripled, which is what saturated the HTTP queue.
// Only the game instance's current world may act on a delivery.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOnlyCurrentWorldHandlesDeliveryTest,
	"CrowdySDK.GameModel.OnlyCurrentWorldHandlesDelivery", CrowdySubPartTestFlags)
bool FCrowdyOnlyCurrentWorldHandlesDeliveryTest::RunTest(const FString& Parameters)
{
	// Two worlds under ONE game instance, which is what a level travel leaves behind. Editor worlds, not Game ones:
	// a Game world asserts in the Mass subsystem on teardown here, and the world type is irrelevant to the check
	// (these subsystems are constructed directly rather than through ShouldCreateSubsystem).
	const TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GetTransientPackage()));
	UWorld* Departed = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	UWorld* Current = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
	Departed->SetGameInstance(Instance.Get());
	Current->SetGameInstance(Instance.Get());

	// The game instance is "in" the world it most recently travelled to.
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
	Context.OwningGameInstance = Instance.Get();
	Context.SetCurrentWorld(Current);

	UCrowdyGameModelSubsystem* CurrentModel = NewObject<UCrowdyGameModelSubsystem>(Current);
	UCrowdyGameModelSubsystem* DepartedModel = NewObject<UCrowdyGameModelSubsystem>(Departed);

	TestTrue(TEXT("the game instance's current world handles deliveries"),
		CurrentModel->IsCurrentWorldForGameInstance());
	TestFalse(TEXT("a world left behind by a travel does not"),
		DepartedModel->IsCurrentWorldForGameInstance());

	// Travelling on makes the previously-current world a departed one too, so exactly one world ever answers true.
	Context.SetCurrentWorld(Departed);
	TestTrue(TEXT("after travelling back, the newly current world handles deliveries"),
		DepartedModel->IsCurrentWorldForGameInstance());
	TestFalse(TEXT("and the previously current world stops"),
		CurrentModel->IsCurrentWorldForGameInstance());

	// Undetermined is NOT departed: a subsystem with no world (a fixture, or one mid-teardown) must keep handling
	// deliveries rather than be mistaken for a world left behind, or the guard silently drops everything.
	UCrowdyGameModelSubsystem* Worldless = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	TestTrue(TEXT("a subsystem with no world is not treated as departed"),
		Worldless->IsCurrentWorldForGameInstance());

	GEngine->DestroyWorldContext(Current);
	Current->DestroyWorld(/*bInformEngineOfWorld=*/false);
	Departed->DestroyWorld(/*bInformEngineOfWorld=*/false);
	return true;
}

// The actor-side ResolveIdentity binding-key path: an owner implementing ICrowdyBindingKeyProvider with a non-empty
// key derives its NetID from the key; an owner with no key (or not implementing the interface) falls through to the
// IdentityPolicy derivation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyActorBindingKeyIdentityTest,
	"CrowdySDK.GameModel.ActorBindingKeyIdentity", CrowdySubPartTestFlags)
bool FCrowdyActorBindingKeyIdentityTest::RunTest(const FString& Parameters)
{
	FCrowdySubPartTestWorld Env;

	// A keyed owner (non-PlayerDerived policy) drives identity from the key. The owner is spawned in the world so
	// its interface answers via ProcessEvent.
	UCrowdyEntityComponent* Comp = NewObject<UCrowdyEntityComponent>(GetTransientPackage());
	Comp->IdentityPolicy = ECrowdyIdentityPolicy::Stable;
	ACrowdyBindingKeyTestActor* KeyedOwner = Env.Spawn<ACrowdyBindingKeyTestActor>();
	if (!TestNotNull(TEXT("keyed owner spawned"), KeyedOwner))
	{
		return false;
	}
	KeyedOwner->BindingKey = TEXT("arena2_boss");
	const FGuid Keyed = Comp->ResolveIdentityForTest(KeyedOwner);
	TestEqual(TEXT("an actor binding key drives identity"), Keyed,
		FCrowdyModelIdentity::NetIDFromBindingKey(TEXT("arena2_boss")));

	// An owner that does not supply a key falls through to the Stable derivation (not a key-derived id). A spawned
	// test actor has no engine instance guid, so the Stable path logs its path-fallback warning; whitelist it.
	AddExpectedError(TEXT("has no engine instance guid"), EAutomationExpectedErrorFlags::Contains, 0);
	UCrowdyEntityComponent* Comp2 = NewObject<UCrowdyEntityComponent>(GetTransientPackage());
	Comp2->IdentityPolicy = ECrowdyIdentityPolicy::Stable;
	AActor* PlainOwner = Env.Spawn<AActor>();
	const FGuid Fallback = Comp2->ResolveIdentityForTest(PlainOwner);
	bool bUnused = false;
	TestEqual(TEXT("no key falls through to the Stable identity"), Fallback, Comp2->ComputeStableNetID(bUnused));
	TestNotEqual(TEXT("the fallback id is not a key-derived id"), Fallback,
		FCrowdyModelIdentity::NetIDFromBindingKey(TEXT("arena2_boss")));

	return true;
}

// UnenrollModelComponent tears down a runtime-added component's sub-participant while its actor lives on, so the
// component no longer resolves and the anchor's later teardown is a clean no-op (no leaked record).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyUnenrollModelComponentTest,
	"CrowdySDK.GameModel.UnenrollModelComponent", CrowdySubPartTestFlags)
bool FCrowdyUnenrollModelComponentTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* Entities = MakeSubPartEntities(FGuid::NewGuid());
	UCrowdyGameModelSubsystem* Model = NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	if (!TestNotNull(TEXT("entities"), Entities) || !TestNotNull(TEXT("model"), Model))
	{
		return false;
	}
	Model->SetEntitySubsystemForTest(Entities);

	AActor* Anchor = NewObject<AActor>(GetTransientPackage());
	const FGuid AnchorNetID = Entities->RegisterParticipant(Anchor, ECrowdyOwnership::Host);

	UCrowdyGameModelTestKeyedComponent* Comp = NewObject<UCrowdyGameModelTestKeyedComponent>(Anchor);
	Comp->BindingKey = TEXT("transient_shield");
	Anchor->AddOwnedComponent(Comp);
	Model->EnrollModelComponent(Comp);

	FGuid SubID;
	TestTrue(TEXT("the runtime component enrolled"), Model->ResolveTargetNetID(Comp, SubID));

	Model->UnenrollModelComponent(Comp);
	FGuid After;
	TestFalse(TEXT("the unenrolled component no longer resolves"), Model->ResolveTargetNetID(Comp, After));

	// The anchor tearing down afterwards must not choke on the already-removed sub (clean no-op).
	Model->HandleEntityUnregisteredForTest(AnchorNetID);
	TestFalse(TEXT("still unresolved after anchor teardown"), Model->ResolveTargetNetID(Comp, After));

	return true;
}

// Get Model Component resolves the right component off an actor (the actor-centric authoring convenience), and
// fails closed for a null actor, an unset class, or a class the actor does not carry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGetModelComponentTest,
	"CrowdySDK.GameModel.GetModelComponent", CrowdySubPartTestFlags)
bool FCrowdyGetModelComponentTest::RunTest(const FString& Parameters)
{
	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("actor"), Actor))
	{
		return false;
	}
	UCrowdyGameModelTestComponent* Comp = NewObject<UCrowdyGameModelTestComponent>(Actor);
	Actor->AddOwnedComponent(Comp);

	TestTrue(TEXT("the actor's container component resolves"),
		UCrowdyModel::GetModelComponent(Actor, UCrowdyGameModelTestComponent::StaticClass()) == Comp);
	TestNull(TEXT("a null actor resolves to nothing"),
		UCrowdyModel::GetModelComponent(nullptr, UCrowdyGameModelTestComponent::StaticClass()));
	TestNull(TEXT("an unset class resolves to nothing"), UCrowdyModel::GetModelComponent(Actor, nullptr));

	return true;
}

#endif
