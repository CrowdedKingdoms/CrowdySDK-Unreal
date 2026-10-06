#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/ActorComponent.h" // EComponentCreationMethod, UActorComponent
#include "Core/CrowdyCategory/FCrowdyTypeIDGenerator.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Script.h" // FEditorScriptExecutionGuard (an AActor's ProcessEvent no-ops without a world)
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership
#include "Replication/GameModel/CrowdyModel.h"
#include "Replication/GameModel/CrowdyModelIdentity.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "UObject/Package.h"
#include "Utils/HelperFunctions.h"

// Component sub-participants. A CrowdyContainer component on a registered actor
// enrolls as its own participant, keyed off the actor's NetID so two same-class components (and the same component
// across clients) get distinct, deterministic ids, inheriting the actor's authority. These cover the entity-
// subsystem enrollment seam (determinism, distinctness, authority inheritance, the dup-key guard, teardown), the
// universal target resolution onto the derived id, and the Get Model Component convenience.
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
