#include "Replication/Components/CrowdyEntityComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Data/CrowdyEntityTypes.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"
#include "Utils/HelperFunctions.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyAuthorityTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// An editor world, like the other headless fixtures: a Game world runs the engine's play initialisation and
	// asserts in the Mass subsystem. An editor world never dispatches BeginPlay at spawn, so a test decides the
	// order of BeginPlay and possession itself, which is exactly what these cases turn on.
	struct FCrowdyPossessionTestWorld
	{
		UWorld* World = nullptr;

		FCrowdyPossessionTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld=*/false);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Editor);
			Context.SetCurrentWorld(World);
		}

		~FCrowdyPossessionTestWorld()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(/*bInformEngineOfWorld=*/false);
		}
	};

	constexpr int64 PossessionTestUserID = 4242;

	// The collaborators an editor world does not create: a bare entity subsystem with no local player id yet, and a
	// game session signed in as PossessionTestUserID with no session UUID yet, so a test can see both get set.
	struct FCrowdyPossessionCollaborators
	{
		UCrowdyEntitySubsystem* Entities = nullptr;
		UCrowdyGameSession* Session = nullptr;

		FCrowdyPossessionCollaborators()
		{
			Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
			UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
			Session = NewObject<UCrowdyGameSession>(GameInstance);
			Session->SetUserID(PossessionTestUserID);
		}

		FGuid ExpectedNetID() const { return UHelperFunctions::GetDeterministicID(PossessionTestUserID); }
	};

	ACrowdyPlayerDerivedTestPawn* SpawnPlayerDerivedPawn(UWorld* World, const FCrowdyPossessionCollaborators& Collaborators)
	{
		ACrowdyPlayerDerivedTestPawn* Pawn = World->SpawnActor<ACrowdyPlayerDerivedTestPawn>();
		if (Pawn)
		{
			Pawn->Entity->SetCollaboratorsForTest(Collaborators.Entities, Collaborators.Session);
		}
		return Pawn;
	}

	// A player controller that answers "local": with no net driver the engine decides that by whether the
	// controller's Player is a ULocalPlayer, so a bare one is attached directly. SetPlayer would also start the
	// input system, which has no viewport here.
	APlayerController* SpawnLocalPlayerController(UWorld* World)
	{
		APlayerController* Controller = World->SpawnActor<APlayerController>();
		if (Controller)
		{
			Controller->Player = NewObject<ULocalPlayer>(GEngine);
		}
		return Controller;
	}
}

// Ownership=Host derives a HostOwned role with no owner id (world entity); Ownership=LocalClient derives Owner with
// the local player id. This is the world-free equivalent of ResolveIdentity's authority tail. Enums are compared as
// their underlying int (the codebase's TestEqual idiom for enum/flags).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateAuthorityDerivationTest,
	"CrowdySDK.State.AuthorityDerivation", CrowdyAuthorityTestFlags)
bool FCrowdyStateAuthorityDerivationTest::RunTest(const FString& Parameters)
{
	const FGuid LocalPlayer = FGuid::NewGuid();

	ECrowdyRole Role = ECrowdyRole::None;
	FGuid OwnerID = FGuid::NewGuid(); // deliberately non-empty first, to prove Host zeroes it
	UCrowdyEntityComponent::DeriveAuthority(ECrowdyOwnership::Host, LocalPlayer, Role, OwnerID);
	TestEqual(TEXT("Host -> HostOwned role"), static_cast<uint8>(Role), static_cast<uint8>(ECrowdyRole::HostOwned));
	TestFalse(TEXT("Host -> no owner id"), OwnerID.IsValid());

	UCrowdyEntityComponent::DeriveAuthority(ECrowdyOwnership::LocalClient, LocalPlayer, Role, OwnerID);
	TestEqual(TEXT("LocalClient -> Owner role"), static_cast<uint8>(Role), static_cast<uint8>(ECrowdyRole::Owner));
	TestEqual(TEXT("LocalClient -> owner id is the local player"), OwnerID, LocalPlayer);
	return true;
}

// The authoring defaults an unedited component ships with: a level-placed actor is host-owned and event-only, and
// the host may override a client-owned entity's view state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateAuthorityDefaultsTest,
	"CrowdySDK.State.AuthorityDefaults", CrowdyAuthorityTestFlags)
bool FCrowdyStateAuthorityDefaultsTest::RunTest(const FString& Parameters)
{
	const UCrowdyEntityComponent* CDO = GetDefault<UCrowdyEntityComponent>();
	if (!TestNotNull(TEXT("component CDO"), CDO))
	{
		return false;
	}
	TestEqual(TEXT("Ownership defaults to Host"),
		static_cast<uint8>(CDO->GetOwnership()), static_cast<uint8>(ECrowdyOwnership::Host));
	TestEqual(TEXT("Mode defaults to Static"),
		static_cast<uint8>(CDO->GetMode()), static_cast<uint8>(ECrowdyEntityMode::Static));
	TestEqual(TEXT("HostOverride defaults to Allow"),
		static_cast<uint8>(CDO->GetHostOverridePolicy()), static_cast<uint8>(ECrowdyHostOverride::Allow));
	return true;
}

// Authored ownership applies to an actor placed in the level; an actor spawned at runtime belongs to the client
// that spawned it whatever the asset says. The signal is the engine's "loaded directly from the map" flag, which
// is set for every level-placed actor in every configuration, so a test can set or clear it on a bare actor and
// drive both branches end to end through identity resolution without a world.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityEffectiveOwnershipTest,
	"CrowdySDK.Entity.EffectiveOwnership", CrowdyAuthorityTestFlags)
bool FCrowdyEntityEffectiveOwnershipTest::RunTest(const FString& Parameters)
{
	using UEC = UCrowdyEntityComponent;

	TestEqual(TEXT("level-placed keeps authored Host"),
		static_cast<uint8>(UEC::ResolveEffectiveOwnership(ECrowdyOwnership::Host, true)),
		static_cast<uint8>(ECrowdyOwnership::Host));
	TestEqual(TEXT("runtime-spawned overrides authored Host to LocalClient"),
		static_cast<uint8>(UEC::ResolveEffectiveOwnership(ECrowdyOwnership::Host, false)),
		static_cast<uint8>(ECrowdyOwnership::LocalClient));
	TestEqual(TEXT("level-placed keeps authored LocalClient"),
		static_cast<uint8>(UEC::ResolveEffectiveOwnership(ECrowdyOwnership::LocalClient, true)),
		static_cast<uint8>(ECrowdyOwnership::LocalClient));
	TestEqual(TEXT("runtime-spawned keeps authored LocalClient"),
		static_cast<uint8>(UEC::ResolveEffectiveOwnership(ECrowdyOwnership::LocalClient, false)),
		static_cast<uint8>(ECrowdyOwnership::LocalClient));

	TestFalse(TEXT("a null actor has no placement guid"), UEC::ResolveActorInstanceGuid(nullptr).IsValid());

	// A runtime spawn: the flag is clear, so authored Host does not apply and this client owns what it spawned.
	AActor* SpawnedActor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* SpawnedComponent = NewObject<UCrowdyEntityComponent>(SpawnedActor);
	if (!TestNotNull(TEXT("runtime-spawn fixture created"), SpawnedComponent))
	{
		return false;
	}
	if (!TestFalse(TEXT("the fixture starts out not flagged as loaded from the map"), SpawnedActor->IsNetStartupActor()))
	{
		return false;
	}
	SpawnedComponent->Ownership = ECrowdyOwnership::Host;
	SpawnedComponent->ResolveIdentityForTest(SpawnedActor);
	TestEqual(TEXT("authored Host on a runtime-spawned actor resolves to Owner"),
		static_cast<uint8>(SpawnedComponent->GetRole()), static_cast<uint8>(ECrowdyRole::Owner));

	// The same asset placed in the level: the flag is set, so the authored Host ownership stands and the entity is
	// host-owned with no per-client owner id. This is the pair that must not disagree between editor and packaged
	// builds, which is why the flag - and not the editor-only placement guid - decides it.
	AActor* PlacedActor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* PlacedComponent = NewObject<UCrowdyEntityComponent>(PlacedActor);
	if (!TestNotNull(TEXT("level-placed fixture created"), PlacedComponent))
	{
		return false;
	}
	PlacedActor->bNetStartup = 1;
	PlacedComponent->Ownership = ECrowdyOwnership::Host;
	PlacedComponent->ResolveIdentityForTest(PlacedActor);
	TestEqual(TEXT("authored Host on a level-placed actor resolves to HostOwned"),
		static_cast<uint8>(PlacedComponent->GetRole()), static_cast<uint8>(ECrowdyRole::HostOwned));
	TestFalse(TEXT("a host-owned entity carries no per-client owner id"), PlacedComponent->GetOwnerID().IsValid());

	// Authored LocalClient is unaffected by placement: it is this client's either way.
	PlacedComponent->Ownership = ECrowdyOwnership::LocalClient;
	PlacedComponent->ResolveIdentityForTest(PlacedActor);
	TestEqual(TEXT("authored LocalClient on a level-placed actor stays Owner"),
		static_cast<uint8>(PlacedComponent->GetRole()), static_cast<uint8>(ECrowdyRole::Owner));
	return true;
}

// IsLocallyOwned is the single rule the Blueprint "is locally controlled" helper delegates to: an entity we own,
// or a host-owned world entity on the elected host. With no entity subsystem to name a host, the host-owned case
// fails closed, and an entity with no role is never locally owned.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityLocallyOwnedTest,
	"CrowdySDK.Entity.LocallyOwned", CrowdyAuthorityTestFlags)
bool FCrowdyEntityLocallyOwnedTest::RunTest(const FString& Parameters)
{
	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor);
	if (!TestNotNull(TEXT("component created"), Component))
	{
		return false;
	}

	TestFalse(TEXT("an entity with no role is not locally owned"), Component->IsLocallyOwned());

	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid::NewGuid(), ECrowdyRole::Owner, 0);
	TestTrue(TEXT("an entity we own is locally owned"), Component->IsLocallyOwned());

	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid::NewGuid(), ECrowdyRole::RemoteProxy, 0);
	TestFalse(TEXT("a remote proxy is not locally owned"), Component->IsLocallyOwned());

	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid(), ECrowdyRole::HostOwned, 0);
	TestFalse(TEXT("a host-owned entity with no known host fails closed"), Component->IsLocallyOwned());
	return true;
}

// The first ownership announcement fires exactly once per identity, carries the settled owner/role/local answer,
// and says nothing for an actor holding no identity - which is a pooled actor sitting in the pool, whose
// throwaway identity was cleared when it was pooled. Being handed a new identity re-arms it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityOwnershipAssignedInitialTest,
	"CrowdySDK.Entity.OwnershipAssignedInitial", CrowdyAuthorityTestFlags)
bool FCrowdyEntityOwnershipAssignedInitialTest::RunTest(const FString& Parameters)
{
	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor);
	if (!TestNotNull(TEXT("component created"), Component))
	{
		return false;
	}

	// Nothing to announce before an identity exists.
	Component->AnnounceInitialOwnershipForTest();
	TestEqual(TEXT("no announcement without an identity"), Component->OwnershipAnnouncementCount, 0);

	const FGuid NetID = FGuid::NewGuid();
	const FGuid OwnerID = FGuid::NewGuid();
	Component->AssignPooledIdentity(NetID, OwnerID, ECrowdyRole::Owner, 0);

	Component->AnnounceInitialOwnershipForTest();
	TestEqual(TEXT("announced once identity is settled"), Component->OwnershipAnnouncementCount, 1);
	TestEqual(TEXT("announced the entity's owner"), Component->LastAnnouncedOwnerID, OwnerID);
	TestEqual(TEXT("announced the entity's role"),
		static_cast<uint8>(Component->LastAnnouncedRole), static_cast<uint8>(ECrowdyRole::Owner));
	TestTrue(TEXT("announced that we own it"), Component->bLastAnnouncedLocallyOwned);

	// One shot: a second attempt at the first announcement is suppressed.
	Component->AnnounceInitialOwnershipForTest();
	TestEqual(TEXT("the first announcement happens exactly once"), Component->OwnershipAnnouncementCount, 1);

	// Returned to the pool: no identity, so nothing is announced.
	Component->ClearIdentity();
	Component->AnnounceInitialOwnershipForTest();
	TestEqual(TEXT("a pooled actor with no identity announces nothing"), Component->OwnershipAnnouncementCount, 1);

	// Checked back out under a new identity: announced again.
	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid(), ECrowdyRole::HostOwned, 0);
	Component->AnnounceInitialOwnershipForTest();
	TestEqual(TEXT("a re-assigned identity announces again"), Component->OwnershipAnnouncementCount, 2);
	TestEqual(TEXT("announced the new role"),
		static_cast<uint8>(Component->LastAnnouncedRole), static_cast<uint8>(ECrowdyRole::HostOwned));
	TestFalse(TEXT("host-owned with no known host is not locally owned"), Component->bLastAnnouncedLocallyOwned);
	return true;
}

// Every ownership change that actually lands is announced on the entity itself, in all three directions, with
// the post-change owner and role, and always after the systems that track the entity have been re-pointed at the
// new owner - game code told "you own this now" usually writes and marks replicated state immediately, which
// needs the entity already tracked under its new owner. A grant that the compare-and-swap drops changes nothing,
// so it announces nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityOwnershipAssignedOnReassignTest,
	"CrowdySDK.Entity.OwnershipAssignedOnReassign", CrowdyAuthorityTestFlags)
bool FCrowdyEntityOwnershipAssignedOnReassignTest::RunTest(const FString& Parameters)
{
	const FGuid LocalPlayer = FGuid::NewGuid();
	const FGuid OtherPlayer = FGuid::NewGuid();
	const FGuid WrongExpectation = FGuid::NewGuid();

	UCrowdyEntitySubsystem* ES = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
	ES->SetLocalPlayerID(LocalPlayer);

	// This fixture carries a UCrowdyEntityComponent default subobject in Static mode, so the reassignment path
	// runs without the world-dependent continuous channel.
	ACrowdyStateHostOverrideActor* Actor = NewObject<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("actor created"), Actor) || !TestNotNull(TEXT("entity component present"), Actor->Entity.Get()))
	{
		return false;
	}

	const FGuid NetID = FGuid::NewGuid();
	FCrowdyEntityRecord Record;
	Record.NetID = NetID;
	Record.OwnerID = LocalPlayer;
	Record.Role = ECrowdyRole::Owner;
	Record.Participant = Actor;
	ES->RegisterEntity(Record);

	TestEqual(TEXT("registration alone announces nothing on the component"),
		Actor->Entity->OwnershipAnnouncementCount, 0);

	// Transferred away.
	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer);
	TestEqual(TEXT("transferring away announces"), Actor->Entity->OwnershipAnnouncementCount, 1);
	TestEqual(TEXT("announced the new owner"), Actor->Entity->LastAnnouncedOwnerID, OtherPlayer);
	TestEqual(TEXT("announced the proxy role"),
		static_cast<uint8>(Actor->Entity->LastAnnouncedRole), static_cast<uint8>(ECrowdyRole::RemoteProxy));
	TestFalse(TEXT("announced that we no longer own it"), Actor->Entity->bLastAnnouncedLocallyOwned);
	TestTrue(TEXT("announced after the tracking systems were re-pointed"),
		Actor->Entity->LastAnnouncementStep > Actor->Entity->TrackersRepointedStep);

	// Transferred back.
	ES->ReassignOwnership(NetID, LocalPlayer, OtherPlayer);
	TestEqual(TEXT("transferring back announces"), Actor->Entity->OwnershipAnnouncementCount, 2);
	TestEqual(TEXT("announced us as the owner"), Actor->Entity->LastAnnouncedOwnerID, LocalPlayer);
	TestTrue(TEXT("announced that we own it again"), Actor->Entity->bLastAnnouncedLocallyOwned);
	TestTrue(TEXT("the adopt direction also announces after the re-point"),
		Actor->Entity->LastAnnouncementStep > Actor->Entity->TrackersRepointedStep);

	// Handed to the host.
	ES->ReassignOwnership(NetID, FGuid(), LocalPlayer);
	TestEqual(TEXT("handing to the host announces"), Actor->Entity->OwnershipAnnouncementCount, 3);
	TestFalse(TEXT("a host-owned entity announces no per-client owner"), Actor->Entity->LastAnnouncedOwnerID.IsValid());
	TestEqual(TEXT("announced the host-owned role"),
		static_cast<uint8>(Actor->Entity->LastAnnouncedRole), static_cast<uint8>(ECrowdyRole::HostOwned));

	// A stale grant is dropped, so nothing changed and nothing is announced.
	ES->ReassignOwnership(NetID, OtherPlayer, WrongExpectation);
	TestEqual(TEXT("a dropped grant announces nothing"), Actor->Entity->OwnershipAnnouncementCount, 3);
	return true;
}

// DoesOwnershipMatch is the pure core of UCrowdyUtilities::DoesCrowdyEntityOwn. It must answer "does the owner own
// the target" for BOTH player owners and host owners, resolving a HostOwned side (whose OwnerID is Guid::Zero) to
// the concrete host id. World-free, mirroring the derivation test above.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnershipMatchTest,
	"CrowdySDK.State.OwnershipMatch", CrowdyAuthorityTestFlags)
bool FCrowdyOwnershipMatchTest::RunTest(const FString& Parameters)
{
	using UEC = UCrowdyEntityComponent;

	const FGuid HostPlayer   = FGuid::NewGuid(); // the host player's id (== host avatar NetID == GetHostID())
	const FGuid OtherPlayer  = FGuid::NewGuid();
	const FGuid WorldEntity  = FGuid::NewGuid(); // a host-owned world entity's stable NetID (not a player id)
	const FGuid ChildNetID   = FGuid::NewGuid(); // a player-spawned entity's own NetID
	const FGuid Zero;                            // host-owned entities carry Guid::Zero as their OwnerID

	// A player owns an entity it spawned (target stamped with the player's id): the unchanged base case.
	TestTrue(TEXT("player owns its own spawned entity"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, OtherPlayer, ECrowdyRole::Owner, OtherPlayer, Zero));

	// A player does not own another player's entity.
	TestFalse(TEXT("player does not own another player's entity"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, OtherPlayer, ECrowdyRole::Owner, HostPlayer, Zero));

	// Sibling entities of the same player do not own each other (the owner acts under its own NetID, not OwnerID).
	TestFalse(TEXT("siblings of the same player do not own each other"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, ChildNetID, ECrowdyRole::Owner, OtherPlayer, Zero));

	// The host PLAYER owns a host-owned world entity (previously impossible: the target's OwnerID was Guid::Zero).
	TestTrue(TEXT("host player owns a host-owned world entity"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, HostPlayer, ECrowdyRole::HostOwned, Zero, HostPlayer));

	// A non-host player does not own a host-owned world entity.
	TestFalse(TEXT("non-host player does not own a host-owned world entity"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, OtherPlayer, ECrowdyRole::HostOwned, Zero, HostPlayer));

	// A host-owned entity, as the owner, acts as the host, so it owns what the host owns (e.g. host-spawned actors).
	TestTrue(TEXT("host-owned entity owns a host-spawned entity"),
		UEC::DoesOwnershipMatch(ECrowdyRole::HostOwned, WorldEntity, ECrowdyRole::Owner, HostPlayer, HostPlayer));

	// Host-owned entities all resolve to the same host id, so they mutually "own" each other. Broad by design (the
	// model has no per-world-entity owner id) and documented as such.
	TestTrue(TEXT("host-owned entities share the host as owner"),
		UEC::DoesOwnershipMatch(ECrowdyRole::HostOwned, WorldEntity, ECrowdyRole::HostOwned, Zero, HostPlayer));

	// Fail closed: with no host elected (invalid host id), a host-owned side never matches.
	TestFalse(TEXT("host-owned target with no host id fails closed"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, HostPlayer, ECrowdyRole::HostOwned, Zero, Zero));
	TestFalse(TEXT("host-owned owner with no host id fails closed"),
		UEC::DoesOwnershipMatch(ECrowdyRole::HostOwned, WorldEntity, ECrowdyRole::Owner, HostPlayer, Zero));

	// An unregistered target (role None, no owner id) is never owned.
	TestFalse(TEXT("unregistered target is never owned"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, HostPlayer, ECrowdyRole::None, Zero, HostPlayer));

	return true;
}

// The one shape that waits for possession is a self-resolving Player Derived pawn with no controller. Every other
// input keeps today's BeginPlay-time registration: an injected identity never resolves, another policy does not
// read a controller, a non-pawn can never be possessed, and a pawn already possessed can resolve right away.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityDeferIdentityToPossessionTest,
	"CrowdySDK.Entity.DeferIdentityToPossession", CrowdyAuthorityTestFlags)
bool FCrowdyEntityDeferIdentityToPossessionTest::RunTest(const FString& Parameters)
{
	using UEC = UCrowdyEntityComponent;

	TestTrue(TEXT("an unpossessed Player Derived pawn waits"),
		UEC::ShouldDeferIdentityToPossession(false, ECrowdyIdentityPolicy::PlayerDerived, true, false));
	TestFalse(TEXT("a pawn possessed before BeginPlay resolves now"),
		UEC::ShouldDeferIdentityToPossession(false, ECrowdyIdentityPolicy::PlayerDerived, true, true));
	TestFalse(TEXT("an injected identity never resolves, so it never waits"),
		UEC::ShouldDeferIdentityToPossession(true, ECrowdyIdentityPolicy::PlayerDerived, true, false));
	TestFalse(TEXT("a Stable actor does not read a controller"),
		UEC::ShouldDeferIdentityToPossession(false, ECrowdyIdentityPolicy::Stable, true, false));
	TestFalse(TEXT("a Random actor does not read a controller"),
		UEC::ShouldDeferIdentityToPossession(false, ECrowdyIdentityPolicy::Random, true, false));
	TestFalse(TEXT("a non-pawn can never be possessed, so it resolves (and falls back) now"),
		UEC::ShouldDeferIdentityToPossession(false, ECrowdyIdentityPolicy::PlayerDerived, false, false));
	return true;
}

// The defect: the engine possesses a pawn spawned during play only after that spawn has dispatched BeginPlay, so a
// Player Derived pawn that resolved at BeginPlay found no controller and registered under a random id, and the
// session UUID every local-player reader keys on was never set. Now the pawn holds no identity and registers
// nothing until its first possession, at which point it registers under the account-derived id and sets the UUID.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedRegistersOnPossessionTest,
	"CrowdySDK.Entity.PlayerDerivedRegistersOnPossession", CrowdyAuthorityTestFlags)
bool FCrowdyEntityPlayerDerivedRegistersOnPossessionTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	ACrowdyPlayerDerivedTestPawn* Pawn = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	if (!TestNotNull(TEXT("the pawn spawned"), Pawn))
	{
		return false;
	}

	Pawn->DispatchBeginPlay();

	TestFalse(TEXT("before possession: no identity"), Pawn->Entity->GetNetID().IsValid());
	TestFalse(TEXT("before possession: not registered"), Collaborators.Entities->FindEntityID(Pawn).IsValid());
	TestFalse(TEXT("before possession: the session UUID is untouched"), Collaborators.Session->GetID().IsValid());
	TestEqual(TEXT("before possession: nothing resolved"), Pawn->Entity->IdentityResolutionCount, 0);
	TestTrue(TEXT("before possession: the pawn's controller change is being listened for"),
		Pawn->ReceiveControllerChangedDelegate.IsBound());

	APlayerController* Controller = SpawnLocalPlayerController(TestWorld.World);
	if (!TestNotNull(TEXT("the player controller spawned"), Controller))
	{
		return false;
	}
	Pawn->PossessedBy(Controller);

	const FGuid Expected = Collaborators.ExpectedNetID();
	TestEqual(TEXT("after possession: registered under the account-derived id"),
		Collaborators.Entities->FindEntityID(Pawn), Expected);
	TestEqual(TEXT("after possession: the component holds that id"), Pawn->Entity->GetNetID(), Expected);
	TestEqual(TEXT("after possession: this client owns its own pawn"),
		static_cast<uint8>(Pawn->Entity->GetRole()), static_cast<uint8>(ECrowdyRole::Owner));
	TestEqual(TEXT("after possession: the session UUID is the same id"), Collaborators.Session->GetID(), Expected);
	TestEqual(TEXT("after possession: resolved exactly once"), Pawn->Entity->IdentityResolutionCount, 1);
	TestFalse(TEXT("after possession: the listener is gone"), Pawn->ReceiveControllerChangedDelegate.IsBound());
	return true;
}

// Control: the first-frame default pawn is possessed before the world begins play, so its controller is present at
// BeginPlay and it resolves there as it always has. Nothing is bound and nothing waits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedPossessedBeforeBeginPlayTest,
	"CrowdySDK.Entity.PlayerDerivedPossessedBeforeBeginPlay", CrowdyAuthorityTestFlags)
bool FCrowdyEntityPlayerDerivedPossessedBeforeBeginPlayTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	ACrowdyPlayerDerivedTestPawn* Pawn = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	APlayerController* Controller = SpawnLocalPlayerController(TestWorld.World);
	if (!TestNotNull(TEXT("the pawn spawned"), Pawn) || !TestNotNull(TEXT("the player controller spawned"), Controller))
	{
		return false;
	}

	Pawn->PossessedBy(Controller);
	TestEqual(TEXT("possession before BeginPlay resolves nothing yet"), Pawn->Entity->IdentityResolutionCount, 0);

	Pawn->DispatchBeginPlay();

	const FGuid Expected = Collaborators.ExpectedNetID();
	TestEqual(TEXT("BeginPlay registers under the account-derived id"), Collaborators.Entities->FindEntityID(Pawn), Expected);
	TestEqual(TEXT("BeginPlay sets the session UUID"), Collaborators.Session->GetID(), Expected);
	TestEqual(TEXT("resolved exactly once, at BeginPlay"), Pawn->Entity->IdentityResolutionCount, 1);
	TestFalse(TEXT("nothing was bound to the controller change"), Pawn->ReceiveControllerChangedDelegate.IsBound());
	return true;
}

// The identity is the account, not the controller: once resolved, an unpossess and a possession by another player
// controller re-register nothing and change nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedSecondPossessionTest,
	"CrowdySDK.Entity.PlayerDerivedSecondPossession", CrowdyAuthorityTestFlags)
bool FCrowdyEntityPlayerDerivedSecondPossessionTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	ACrowdyPlayerDerivedTestPawn* Pawn = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	APlayerController* First = SpawnLocalPlayerController(TestWorld.World);
	APlayerController* Second = SpawnLocalPlayerController(TestWorld.World);
	if (!TestNotNull(TEXT("the pawn spawned"), Pawn) || !TestNotNull(TEXT("the first controller spawned"), First)
		|| !TestNotNull(TEXT("the second controller spawned"), Second))
	{
		return false;
	}

	Pawn->DispatchBeginPlay();
	Pawn->PossessedBy(First);
	const FGuid Resolved = Pawn->Entity->GetNetID();
	TestTrue(TEXT("the first possession resolved"), Resolved.IsValid());

	Pawn->UnPossessed();
	TestEqual(TEXT("an unpossess changes nothing"), Pawn->Entity->GetNetID(), Resolved);

	Pawn->PossessedBy(Second);
	TestEqual(TEXT("a second possession keeps the id"), Pawn->Entity->GetNetID(), Resolved);
	TestEqual(TEXT("a second possession is still registered under it"), Collaborators.Entities->FindEntityID(Pawn), Resolved);
	TestEqual(TEXT("a second possession does not resolve again"), Pawn->Entity->IdentityResolutionCount, 1);
	TestFalse(TEXT("nothing is listening any more"), Pawn->ReceiveControllerChangedDelegate.IsBound());
	return true;
}

// A respawn that keeps the previous pawn alive after unpossession (DetachFromControllerPendingDestroy plus a
// lifespan): the new pawn derives the same account id, takes it from the corpse, and keeps it once the corpse ends play.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedRespawnSupersedesCorpseTest,
	"CrowdySDK.Entity.PlayerDerivedRespawnSupersedesCorpse", CrowdyAuthorityTestFlags)
bool FCrowdyEntityPlayerDerivedRespawnSupersedesCorpseTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	ACrowdyPlayerDerivedTestPawn* First = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	ACrowdyPlayerDerivedTestPawn* Second = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	APlayerController* Controller = SpawnLocalPlayerController(TestWorld.World);
	if (!TestNotNull(TEXT("the first pawn spawned"), First) || !TestNotNull(TEXT("the second pawn spawned"), Second)
		|| !TestNotNull(TEXT("the controller spawned"), Controller))
	{
		return false;
	}
	const FGuid Expected = Collaborators.ExpectedNetID();

	First->DispatchBeginPlay();
	First->PossessedBy(Controller);
	TestEqual(TEXT("the first pawn is registered under the account id"), Collaborators.Entities->FindEntityID(First), Expected);

	First->UnPossessed();
	Second->DispatchBeginPlay();
	Second->PossessedBy(Controller);

	TestEqual(TEXT("the second pawn is registered under the account id"), Collaborators.Entities->FindEntityID(Second), Expected);
	TestEqual(TEXT("the registry's participant for the account id is the second pawn"),
		Collaborators.Entities->FindParticipant(Expected), static_cast<UObject*>(Second));
	TestFalse(TEXT("the corpse no longer holds an id"), Collaborators.Entities->FindEntityID(First).IsValid());
	TestFalse(TEXT("the corpse's component forgot its id"), First->Entity->GetNetID().IsValid());

	First->Entity->DestroyComponent();
	TestEqual(TEXT("the corpse ending play leaves the live pawn registered"), Collaborators.Entities->FindEntityID(Second), Expected);
	TestNotNull(TEXT("the record still resolves after the corpse is gone"), Collaborators.Entities->FindRecord(Expected));
	return true;
}

// ClearIdentity on a pawn still waiting for its controller drops the wait too, so a later possession resolves nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedClearIdentityWhileWaitingTest,
	"CrowdySDK.Entity.PlayerDerivedClearIdentityWhileWaiting", CrowdyAuthorityTestFlags)
bool FCrowdyEntityPlayerDerivedClearIdentityWhileWaitingTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	ACrowdyPlayerDerivedTestPawn* Pawn = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	APlayerController* Controller = SpawnLocalPlayerController(TestWorld.World);
	if (!TestNotNull(TEXT("the pawn spawned"), Pawn) || !TestNotNull(TEXT("the controller spawned"), Controller))
	{
		return false;
	}

	Pawn->DispatchBeginPlay();
	Pawn->Entity->ClearIdentity();
	TestFalse(TEXT("clearing the identity stops the wait"), Pawn->ReceiveControllerChangedDelegate.IsBound());

	Pawn->PossessedBy(Controller);
	TestEqual(TEXT("a possession after the clear resolves nothing"), Pawn->Entity->IdentityResolutionCount, 0);
	TestFalse(TEXT("nothing was registered"), Collaborators.Entities->FindEntityID(Pawn).IsValid());
	return true;
}

// A pawn that ends play before anyone possesses it leaves nothing behind: no record, no session UUID, no listener on
// the pawn. It does say so, because a Player Derived pawn that was never registered is worth a line in the log. The
// component is destroyed directly: an editor world never initialises its actors, so AActor::Destroy would not route
// EndPlay there, while DestroyComponent runs the same EndPlay with the same Destroyed reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedEndPlayBeforePossessionTest,
	"CrowdySDK.Entity.PlayerDerivedEndPlayBeforePossession", CrowdyAuthorityTestFlags)
bool FCrowdyEntityPlayerDerivedEndPlayBeforePossessionTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	ACrowdyPlayerDerivedTestPawn* Pawn = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	if (!TestNotNull(TEXT("the pawn spawned"), Pawn))
	{
		return false;
	}
	UCrowdyEntityComponent* Component = Pawn->Entity;

	Pawn->DispatchBeginPlay();
	TestTrue(TEXT("the pawn is waiting for a controller"), Pawn->ReceiveControllerChangedDelegate.IsBound());

	AddExpectedMessagePlain(TEXT("was never possessed, so it was never registered"), ELogVerbosity::Warning,
		EAutomationExpectedMessageFlags::Contains, 1);
	Component->DestroyComponent();

	TestFalse(TEXT("the component ended play"), Component->HasBegunPlay());
	TestFalse(TEXT("nothing was registered"), Collaborators.Entities->FindEntityID(Pawn).IsValid());
	TestFalse(TEXT("the session UUID is untouched"), Collaborators.Session->GetID().IsValid());
	TestFalse(TEXT("the listener was removed"), Pawn->ReceiveControllerChangedDelegate.IsBound());
	TestEqual(TEXT("nothing resolved"), Component->IdentityResolutionCount, 0);
	return true;
}

// A remote destroy with a delay unregisters the entity at once and destroys its actor later. If the id is registered
// again meanwhile (a respawn keeping its id), the old actor ending play must leave the new record alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityEndPlayLeavesSuccessorRecordTest,
	"CrowdySDK.Entity.EndPlayLeavesSuccessorRecord", CrowdyAuthorityTestFlags)
bool FCrowdyEntityEndPlayLeavesSuccessorRecordTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	ACrowdyStateHostOverrideActor* Departed = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	ACrowdyStateHostOverrideActor* Registered = TestWorld.World->SpawnActor<ACrowdyStateHostOverrideActor>();
	AActor* Successor = TestWorld.World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("the departed actor spawned"), Departed) || !TestNotNull(TEXT("the registered actor spawned"), Registered)
		|| !TestNotNull(TEXT("the successor spawned"), Successor))
	{
		return false;
	}

	const FGuid Reused = FGuid::NewGuid();
	Departed->Entity->SetCollaboratorsForTest(Collaborators.Entities, Collaborators.Session);
	Departed->Entity->AssignPooledIdentity(Reused, FGuid(), ECrowdyRole::RemoteProxy, CROWDY_INVALID_CLASS_ID);
	Departed->DispatchBeginPlay();
	if (!TestTrue(TEXT("the departed actor registered under its id"), Collaborators.Entities->FindEntity(Reused) == Departed))
	{
		return false;
	}

	Collaborators.Entities->UnregisterEntity(Reused);
	FCrowdyEntityRecord SuccessorRecord;
	SuccessorRecord.NetID = Reused;
	SuccessorRecord.Role = ECrowdyRole::RemoteProxy;
	SuccessorRecord.Participant = Successor;
	Collaborators.Entities->RegisterEntity(SuccessorRecord);

	Departed->Entity->DestroyComponent();
	TestTrue(TEXT("the departed actor ending play leaves the successor's record"), Collaborators.Entities->FindEntity(Reused) == Successor);

	// The control: an actor still named by its record removes it when it ends play.
	const FGuid Own = FGuid::NewGuid();
	Registered->Entity->SetCollaboratorsForTest(Collaborators.Entities, Collaborators.Session);
	Registered->Entity->AssignPooledIdentity(Own, FGuid(), ECrowdyRole::RemoteProxy, CROWDY_INVALID_CLASS_ID);
	Registered->DispatchBeginPlay();
	Registered->Entity->DestroyComponent();
	TestNull(TEXT("an actor's own record goes when it ends play"), Collaborators.Entities->FindRecord(Own));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
