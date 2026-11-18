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
#include "Replication/Components/CrowdySpectatorComponent.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Serialization/CrowdyActorId.h"
#include "Subsystem/CrowdyGameSession.h"
#include "UObject/Package.h"

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

// Ownership=Host derives a WorldOwned role with no owner id (world entity); Ownership=LocalClient derives Owner with
// the local player id. This is the world-free equivalent of ResolveIdentity's authority tail. Enums are compared as
// their underlying int (the codebase's TestEqual idiom for enum/flags).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateAuthorityDerivationTest,
	"CrowdySDK.State.AuthorityDerivation", CrowdyAuthorityTestFlags)
bool FCrowdyStateAuthorityDerivationTest::RunTest(const FString& Parameters)
{
	const FGuid LocalPlayer = FGuid::NewGuid();

	ECrowdyRole Role = ECrowdyRole::None;
	FGuid WriterID = FGuid::NewGuid(); // deliberately non-empty first, to prove Host zeroes it
	UCrowdyEntityComponent::DeriveAuthority(ECrowdyOwnership::Host, LocalPlayer, Role, WriterID);
	TestEqual(TEXT("Host -> WorldOwned role"), static_cast<uint8>(Role), static_cast<uint8>(ECrowdyRole::WorldOwned));
	TestFalse(TEXT("Host -> no owner id"), WriterID.IsValid());

	UCrowdyEntityComponent::DeriveAuthority(ECrowdyOwnership::LocalClient, LocalPlayer, Role, WriterID);
	TestEqual(TEXT("LocalClient -> Owner role"), static_cast<uint8>(Role), static_cast<uint8>(ECrowdyRole::Owner));
	TestEqual(TEXT("LocalClient -> owner id is the local player"), WriterID, LocalPlayer);
	return true;
}

// The authoring defaults an unedited component ships with: a level-placed actor is World-owned and event-only.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateAuthorityDefaultsTest,
	"CrowdySDK.State.AuthorityDefaults", CrowdyAuthorityTestFlags)
bool FCrowdyStateAuthorityDefaultsTest::RunTest(const FString& Parameters)
{
	const UCrowdyEntityComponent* CDO = GetDefault<UCrowdyEntityComponent>();
	if (!TestNotNull(TEXT("component CDO"), CDO))
	{
		return false;
	}
	TestEqual(TEXT("Owned By defaults to Auto"),
		static_cast<uint8>(CDO->OwnedBy), static_cast<uint8>(ECrowdyOwnedBy::Auto));
	TestEqual(TEXT("Replicate Movement defaults to Auto"),
		static_cast<uint8>(CDO->ReplicateMovement), static_cast<uint8>(ECrowdyReplicateMovement::Auto));
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

	// A runtime spawn: the flag is clear, so authored World does not apply and this client owns what it spawned.
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
	SpawnedComponent->OwnedBy = ECrowdyOwnedBy::World;
	SpawnedComponent->ResolveIdentityForTest(SpawnedActor);
	TestEqual(TEXT("authored World on a runtime-spawned actor resolves to Owner"),
		static_cast<uint8>(SpawnedComponent->GetRole()), static_cast<uint8>(ECrowdyRole::Owner));

	// The same asset placed in the level: the flag is set, so the authored World ownership stands and the entity is
	// world-owned with no per-client owner id. This is the pair that must not disagree between editor and packaged
	// builds, which is why the flag - and not the editor-only placement guid - decides it.
	AActor* PlacedActor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* PlacedComponent = NewObject<UCrowdyEntityComponent>(PlacedActor);
	if (!TestNotNull(TEXT("level-placed fixture created"), PlacedComponent))
	{
		return false;
	}
	PlacedActor->bNetStartup = 1;
	PlacedComponent->OwnedBy = ECrowdyOwnedBy::World;
	PlacedComponent->ResolveIdentityForTest(PlacedActor);
	TestEqual(TEXT("authored World on a level-placed actor resolves to WorldOwned"),
		static_cast<uint8>(PlacedComponent->GetRole()), static_cast<uint8>(ECrowdyRole::WorldOwned));
	TestFalse(TEXT("a world-owned entity carries no per-client owner id"), PlacedComponent->GetWriterID().IsValid());

	// Authored Player is unaffected by placement: it is this client's either way.
	PlacedComponent->OwnedBy = ECrowdyOwnedBy::Player;
	PlacedComponent->ResolveIdentityForTest(PlacedActor);
	TestEqual(TEXT("authored Player on a level-placed actor stays Owner"),
		static_cast<uint8>(PlacedComponent->GetRole()), static_cast<uint8>(ECrowdyRole::Owner));
	return true;
}

// Owned By decides the owner; World applies only to an actor placed in the level, and a spawned one belongs to its
// spawner. Auto follows placement alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityOwnedByDecidesOwnerTest,
	"CrowdySDK.Entity.OwnedByDecidesOwner", CrowdyAuthorityTestFlags)
bool FCrowdyEntityOwnedByDecidesOwnerTest::RunTest(const FString& Parameters)
{
	AActor* PlacedActor = NewObject<AActor>(GetTransientPackage());
	AActor* SpawnedActor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Placed = NewObject<UCrowdyEntityComponent>(PlacedActor);
	UCrowdyEntityComponent* Spawned = NewObject<UCrowdyEntityComponent>(SpawnedActor);
	if (!TestNotNull(TEXT("placed fixture"), Placed) || !TestNotNull(TEXT("spawned fixture"), Spawned))
	{
		return false;
	}
	PlacedActor->bNetStartup = 1;

	Placed->OwnedBy = ECrowdyOwnedBy::World;
	Placed->ResolveIdentityForTest(PlacedActor);
	TestEqual(TEXT("World on a placed actor is written by the host"), Placed->GetRole(), ECrowdyRole::WorldOwned);
	TestFalse(TEXT("with no per-client owner id"), Placed->GetWriterID().IsValid());

	Spawned->OwnedBy = ECrowdyOwnedBy::World;
	Spawned->ResolveIdentityForTest(SpawnedActor);
	TestEqual(TEXT("World on a spawned actor belongs to its spawner"), Spawned->GetRole(), ECrowdyRole::Owner);

	Placed->OwnedBy = ECrowdyOwnedBy::Player;
	Placed->ResolveIdentityForTest(PlacedActor);
	TestEqual(TEXT("Player on a placed actor is this client's"), Placed->GetRole(), ECrowdyRole::Owner);

	Placed->OwnedBy = ECrowdyOwnedBy::Auto;
	Placed->ResolveIdentityForTest(PlacedActor);
	TestEqual(TEXT("Auto on a placed actor is the world's"), Placed->GetRole(), ECrowdyRole::WorldOwned);

	Spawned->OwnedBy = ECrowdyOwnedBy::Auto;
	Spawned->ResolveIdentityForTest(SpawnedActor);
	TestEqual(TEXT("Auto on a spawned actor is its spawner's"), Spawned->GetRole(), ECrowdyRole::Owner);
	return true;
}

// IsLocallyOwned is the single rule the Blueprint "is locally controlled" helper delegates to: an entity we own.
// A World entity has no writer, and an entity with no role is never locally owned.
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

	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid::NewGuid(), 0, ECrowdyRole::Owner, 0);
	TestTrue(TEXT("an entity we own is locally owned"), Component->IsLocallyOwned());

	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid::NewGuid(), 0, ECrowdyRole::RemoteProxy, 0);
	TestFalse(TEXT("a remote proxy is not locally owned"), Component->IsLocallyOwned());

	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid(), 0, ECrowdyRole::WorldOwned, 0);
	TestFalse(TEXT("a World entity is not locally owned"), Component->IsLocallyOwned());
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
	const FGuid WriterID = FGuid::NewGuid();
	Component->AssignPooledIdentity(NetID, WriterID, 0, ECrowdyRole::Owner, 0);

	Component->AnnounceInitialOwnershipForTest();
	TestEqual(TEXT("announced once identity is settled"), Component->OwnershipAnnouncementCount, 1);
	TestEqual(TEXT("announced the entity's owner"), Component->LastAnnouncedWriterID, WriterID);
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
	Component->AssignPooledIdentity(FGuid::NewGuid(), FGuid(), 0, ECrowdyRole::WorldOwned, 0);
	Component->AnnounceInitialOwnershipForTest();
	TestEqual(TEXT("a re-assigned identity announces again"), Component->OwnershipAnnouncementCount, 2);
	TestEqual(TEXT("announced the new role"),
		static_cast<uint8>(Component->LastAnnouncedRole), static_cast<uint8>(ECrowdyRole::WorldOwned));
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
	Record.WriterID = LocalPlayer;
	Record.Role = ECrowdyRole::Owner;
	Record.Participant = Actor;
	ES->RegisterEntity(Record);

	TestEqual(TEXT("registration alone announces nothing on the component"),
		Actor->Entity->OwnershipAnnouncementCount, 0);

	// Transferred away.
	ES->ReassignOwnership(NetID, OtherPlayer, LocalPlayer, 0);
	TestEqual(TEXT("transferring away announces"), Actor->Entity->OwnershipAnnouncementCount, 1);
	TestEqual(TEXT("announced the new owner"), Actor->Entity->LastAnnouncedWriterID, OtherPlayer);
	TestEqual(TEXT("announced the proxy role"),
		static_cast<uint8>(Actor->Entity->LastAnnouncedRole), static_cast<uint8>(ECrowdyRole::RemoteProxy));
	TestFalse(TEXT("announced that we no longer own it"), Actor->Entity->bLastAnnouncedLocallyOwned);
	TestTrue(TEXT("announced after the tracking systems were re-pointed"),
		Actor->Entity->LastAnnouncementStep > Actor->Entity->TrackersRepointedStep);

	// Transferred back.
	ES->ReassignOwnership(NetID, LocalPlayer, OtherPlayer, 0);
	TestEqual(TEXT("transferring back announces"), Actor->Entity->OwnershipAnnouncementCount, 2);
	TestEqual(TEXT("announced us as the owner"), Actor->Entity->LastAnnouncedWriterID, LocalPlayer);
	TestTrue(TEXT("announced that we own it again"), Actor->Entity->bLastAnnouncedLocallyOwned);
	TestTrue(TEXT("the adopt direction also announces after the re-point"),
		Actor->Entity->LastAnnouncementStep > Actor->Entity->TrackersRepointedStep);

	// Handed to the host.
	ES->ReassignOwnership(NetID, FGuid(), LocalPlayer, 0);
	TestEqual(TEXT("handing to the host announces"), Actor->Entity->OwnershipAnnouncementCount, 3);
	TestFalse(TEXT("a host-owned entity announces no per-client owner"), Actor->Entity->LastAnnouncedWriterID.IsValid());
	TestEqual(TEXT("announced the host-owned role"),
		static_cast<uint8>(Actor->Entity->LastAnnouncedRole), static_cast<uint8>(ECrowdyRole::WorldOwned));

	// A stale grant is dropped, so nothing changed and nothing is announced.
	ES->ReassignOwnership(NetID, OtherPlayer, WrongExpectation, 0);
	TestEqual(TEXT("a dropped grant announces nothing"), Actor->Entity->OwnershipAnnouncementCount, 3);
	return true;
}

// DoesOwnershipMatch is the pure core of UCrowdyUtilities::DoesCrowdyEntityOwn.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyOwnershipMatchTest,
	"CrowdySDK.State.OwnershipMatch", CrowdyAuthorityTestFlags)
bool FCrowdyOwnershipMatchTest::RunTest(const FString& Parameters)
{
	using UEC = UCrowdyEntityComponent;

	const FGuid LeaderPlayer = FGuid::NewGuid();
	const FGuid OtherPlayer  = FGuid::NewGuid();
	const FGuid WorldEntity  = FGuid::NewGuid(); // a World entity's stable NetID (not a player id)
	const FGuid ChildNetID   = FGuid::NewGuid(); // a player-spawned entity's own NetID
	const FGuid Zero;                            // World entities carry Guid::Zero as their WriterID

	TestTrue(TEXT("player owns its own spawned entity"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, OtherPlayer, ECrowdyRole::Owner, OtherPlayer));
	TestFalse(TEXT("player does not own another player's entity"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, OtherPlayer, ECrowdyRole::Owner, LeaderPlayer));
	// The owner acts under its own NetID, not its WriterID.
	TestFalse(TEXT("siblings of the same player do not own each other"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, ChildNetID, ECrowdyRole::Owner, OtherPlayer));

	// Nothing writes a World entity, the Leader included, so it is owned by no one and owns nothing.
	TestFalse(TEXT("no player owns a World entity"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, LeaderPlayer, ECrowdyRole::WorldOwned, Zero));
	TestFalse(TEXT("a World entity owns nothing, even an entity stamped with its id"),
		UEC::DoesOwnershipMatch(ECrowdyRole::WorldOwned, WorldEntity, ECrowdyRole::Owner, WorldEntity));
	TestFalse(TEXT("World entities do not own each other"),
		UEC::DoesOwnershipMatch(ECrowdyRole::WorldOwned, WorldEntity, ECrowdyRole::WorldOwned, Zero));

	TestFalse(TEXT("unregistered target is never owned"),
		UEC::DoesOwnershipMatch(ECrowdyRole::Owner, LeaderPlayer, ECrowdyRole::None, Zero));

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
// nothing until its first possession, at which point it registers under the session's id and sets the UUID.
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

	const FGuid Expected = Collaborators.Session->GetID();
	TestTrue(TEXT("after possession: the session holds a valid id"), Expected.IsValid());
	TestEqual(TEXT("after possession: registered under the session's id"),
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

	const FGuid Expected = Pawn->Entity->GetNetID();
	TestTrue(TEXT("BeginPlay resolves a valid id"), Expected.IsValid());
	TestEqual(TEXT("BeginPlay registers under it"), Collaborators.Entities->FindEntityID(Pawn), Expected);
	TestEqual(TEXT("BeginPlay sets the session UUID to the same id"), Collaborators.Session->GetID(), Expected);
	TestEqual(TEXT("resolved exactly once, at BeginPlay"), Pawn->Entity->IdentityResolutionCount, 1);
	TestFalse(TEXT("nothing was bound to the controller change"), Pawn->ReceiveControllerChangedDelegate.IsBound());
	return true;
}

// The identity is the sign-in's id, not the controller: once resolved, an unpossess and a possession by another player
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
	TestEqual(TEXT("to the id the session holds"), Collaborators.Session->GetID(), Resolved);
	Pawn->UnPossessed();
	TestEqual(TEXT("an unpossess changes nothing"), Pawn->Entity->GetNetID(), Resolved);

	Pawn->PossessedBy(Second);
	TestEqual(TEXT("a second possession keeps the id"), Pawn->Entity->GetNetID(), Resolved);
	TestEqual(TEXT("and the session's"), Collaborators.Session->GetID(), Resolved);
	TestEqual(TEXT("a second possession is still registered under it"), Collaborators.Entities->FindEntityID(Pawn), Resolved);
	TestEqual(TEXT("a second possession does not resolve again"), Pawn->Entity->IdentityResolutionCount, 1);
	TestFalse(TEXT("nothing is listening any more"), Pawn->ReceiveControllerChangedDelegate.IsBound());
	return true;
}

// Each sign-in gives the session a new id, and the live Player Derived pawn re-keys to it: it stops answering to the old
// id, registers under the new one as the same actor, and writes again. A sign-in as another user over a live one does
// the same.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedRekeysAtSignInTest,
	"CrowdySDK.Entity.PlayerDerivedPawnRekeysAtEachSignIn", CrowdyAuthorityTestFlags)
bool FCrowdyEntityPlayerDerivedRekeysAtSignInTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	Collaborators.Entities->SetGameSessionForTest(Collaborators.Session);
	Collaborators.Entities->BindGameSessionForTest();
	ACrowdyPlayerDerivedTestPawn* Pawn = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	APlayerController* Controller = SpawnLocalPlayerController(TestWorld.World);
	if (!TestNotNull(TEXT("the pawn spawned"), Pawn) || !TestNotNull(TEXT("the controller spawned"), Controller))
	{
		return false;
	}

	Collaborators.Session->BeginSignIn();
	Pawn->DispatchBeginPlay();
	Pawn->PossessedBy(Controller);
	const FGuid First = Pawn->Entity->GetNetID();
	TestTrue(TEXT("the pawn takes the sign-in's id"), First.IsValid() && First == Collaborators.Session->GetID());
	TestTrue(TEXT("and writes"), Pawn->Entity->IsLocallyOwned());

	// A sign-out drops the id with the connection closed, and re-keys nothing until the next sign-in.
	Collaborators.Session->ClearCurrentSessionData();
	TestEqual(TEXT("signed out, the pawn still holds its id"), Pawn->Entity->GetNetID(), First);

	Collaborators.Session->SetUserID(PossessionTestUserID);
	Collaborators.Session->BeginSignIn();
	const FGuid Second = Collaborators.Session->GetID();
	TestTrue(TEXT("the same user signing back in gets a new id"), Second.IsValid() && Second != First);
	TestEqual(TEXT("the pawn re-keys to it"), Pawn->Entity->GetNetID(), Second);
	TestEqual(TEXT("and its owner"), Pawn->Entity->GetWriterID(), Second);
	TestEqual(TEXT("and sends under it"), Pawn->Entity->GetReplicationUUID(), Second.ToString(EGuidFormats::Digits));
	TestEqual(TEXT("registered under the new id"), Collaborators.Entities->FindEntityID(Pawn), Second);
	TestEqual(TEXT("as the same actor"), Collaborators.Entities->FindParticipant(Second), static_cast<UObject*>(Pawn));
	TestNull(TEXT("the old id answers for nothing"), Collaborators.Entities->FindParticipant(First));
	TestTrue(TEXT("the pawn writes again"), Pawn->Entity->IsLocallyOwned() && Collaborators.Entities->IsLocallyOwned(Second));
	TestTrue(TEXT("and the old id is still known as this client's"), Collaborators.Entities->IsLocalSender(First));
	TestEqual(TEXT("its full state waits for the connection"), Collaborators.Entities->GetRekeyedAwaitingConnectionCountForTest(), 1);
	Collaborators.Entities->HandleRealtimeUpForTest();
	TestEqual(TEXT("and goes once it is up"), Collaborators.Entities->GetRekeyedAwaitingConnectionCountForTest(), 0);

	// Another user signing in over the live sign-in.
	Collaborators.Session->EndSignIn();
	Collaborators.Session->SetUserID(PossessionTestUserID + 1);
	Collaborators.Session->BeginSignIn();
	const FGuid Third = Collaborators.Session->GetID();
	TestTrue(TEXT("another user's sign-in gets another id"), Third.IsValid() && Third != Second);
	TestEqual(TEXT("and the pawn re-keys to it too"), Collaborators.Entities->FindEntityID(Pawn), Third);
	TestTrue(TEXT("writing under it"), Pawn->Entity->IsLocallyOwned());
	TestNull(TEXT("leaving the previous id free"), Collaborators.Entities->FindParticipant(Second));
	return true;
}

// A pawn whose Player Derived identity resolved to the session's id registers as the player's avatar and stays one
// through a re-key; a Player Derived pawn that fell back to a random id is not one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityAvatarFlagFollowsPlayerDerivedIdentityTest,
	"CrowdySDK.Entity.AvatarFlagFollowsPlayerDerivedIdentity", CrowdyAuthorityTestFlags)
bool FCrowdyEntityAvatarFlagFollowsPlayerDerivedIdentityTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	Collaborators.Entities->SetGameSessionForTest(Collaborators.Session);
	Collaborators.Entities->BindGameSessionForTest();
	ACrowdyPlayerDerivedTestPawn* Pawn = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	ACrowdyPlayerDerivedTestPawn* Fallback = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	APlayerController* Controller = SpawnLocalPlayerController(TestWorld.World);
	if (!TestNotNull(TEXT("the pawn spawned"), Pawn) || !TestNotNull(TEXT("the fallback pawn spawned"), Fallback)
		|| !TestNotNull(TEXT("the controller spawned"), Controller))
	{
		return false;
	}

	// With no game session to read an id from, Player Derived falls back to a random one.
	Fallback->Entity->SetCollaboratorsForTest(Collaborators.Entities, nullptr);

	Collaborators.Session->BeginSignIn();
	Pawn->DispatchBeginPlay();
	Pawn->PossessedBy(Controller);
	const FCrowdyEntityRecord* Record = Collaborators.Entities->FindRecord(Pawn->Entity->GetNetID());
	if (!TestNotNull(TEXT("the pawn registered"), Record))
	{
		return false;
	}
	TestTrue(TEXT("a pawn under the session's id is the player's avatar"), Record->bAvatar);
	TestEqual(TEXT("sending under its bare id"), Pawn->Entity->GetReplicationUUID(), Record->NetID.ToString(EGuidFormats::Digits));

	Collaborators.Session->ClearCurrentSessionData();
	Collaborators.Session->SetUserID(PossessionTestUserID);
	Collaborators.Session->BeginSignIn();
	const FCrowdyEntityRecord* Rekeyed = Collaborators.Entities->FindRecord(Collaborators.Session->GetID());
	if (!TestNotNull(TEXT("the pawn re-keyed to the next sign-in's id"), Rekeyed))
	{
		return false;
	}
	TestTrue(TEXT("as the same actor"), Rekeyed->GetActor() == Pawn);
	TestTrue(TEXT("and still the player's avatar"), Rekeyed->bAvatar);

	AddExpectedMessagePlain(TEXT("so it fell back to a random id"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Fallback->DispatchBeginPlay();
	Fallback->PossessedBy(Controller);
	const FCrowdyEntityRecord* Random = Collaborators.Entities->FindRecord(Fallback->Entity->GetNetID());
	if (!TestNotNull(TEXT("the fallback pawn registered"), Random))
	{
		return false;
	}
	TestTrue(TEXT("under an id that is not the session's"), Random->NetID != Collaborators.Session->GetID());
	TestFalse(TEXT("so it is not the player's avatar"), Random->bAvatar);
	TestTrue(TEXT("and it sends under its writer's id, so other clients do not draw it as one"), Random->WriterID.IsValid()
		&& Fallback->Entity->GetReplicationUUID() == FCrowdyActorId::ForOwner(Random->NetID, Random->WriterID).ToString());
	return true;
}

// A pawn possessed before anyone signs in mints the session's id, and the first sign-in adopts it, so the pawn keeps
// its registration and nothing re-keys; the sign-in after that replaces it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedIdAdoptedAtSignInTest,
	"CrowdySDK.Entity.PlayerDerivedIdMintedBeforeSignInIsAdopted", CrowdyAuthorityTestFlags)
bool FCrowdyEntityPlayerDerivedIdAdoptedAtSignInTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	Collaborators.Session->ClearCurrentSessionData();
	Collaborators.Entities->SetGameSessionForTest(Collaborators.Session);
	Collaborators.Entities->BindGameSessionForTest();
	ACrowdyPlayerDerivedTestPawn* First = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	ACrowdyPlayerDerivedTestPawn* Second = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	APlayerController* Controller = SpawnLocalPlayerController(TestWorld.World);
	if (!TestNotNull(TEXT("the first pawn spawned"), First) || !TestNotNull(TEXT("the second pawn spawned"), Second)
		|| !TestNotNull(TEXT("the controller spawned"), Controller))
	{
		return false;
	}

	First->DispatchBeginPlay();
	First->PossessedBy(Controller);
	const FGuid Minted = First->Entity->GetNetID();
	TestTrue(TEXT("a pawn resolves an id before sign-in"), Minted.IsValid());
	TestEqual(TEXT("and the session holds it"), Collaborators.Session->GetID(), Minted);

	Collaborators.Session->SetUserID(PossessionTestUserID);
	Collaborators.Session->BeginSignIn();
	TestEqual(TEXT("the first sign-in adopts it"), Collaborators.Session->GetID(), Minted);
	TestEqual(TEXT("so the pawn keeps it"), Collaborators.Entities->FindEntityID(First), Minted);
	TestEqual(TEXT("with nothing re-keyed"), Collaborators.Entities->GetRekeyedAwaitingConnectionCountForTest(), 0);

	First->UnPossessed();
	Second->DispatchBeginPlay();
	Second->PossessedBy(Controller);
	TestEqual(TEXT("a respawn reuses it"), Second->Entity->GetNetID(), Minted);

	Collaborators.Session->ClearCurrentSessionData();
	Collaborators.Session->SetUserID(PossessionTestUserID);
	Collaborators.Session->BeginSignIn();
	TestTrue(TEXT("the next sign-in replaces it"), Collaborators.Session->GetID().IsValid() && Collaborators.Session->GetID() != Minted);
	TestEqual(TEXT("and the live pawn follows"), Collaborators.Entities->FindEntityID(Second), Collaborators.Session->GetID());
	return true;
}

// A spectator with no pawn yet mints the session's id only while signed in, and the pawn spawned later sends under
// that same id instead of a second one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityPlayerDerivedReusesTheSpectatorsIdTest,
	"CrowdySDK.Entity.PlayerDerivedReusesTheIdASpectatorMinted", CrowdyAuthorityTestFlags)
bool FCrowdyEntityPlayerDerivedReusesTheSpectatorsIdTest::RunTest(const FString& Parameters)
{
	FCrowdyPossessionTestWorld TestWorld;
	FCrowdyPossessionCollaborators Collaborators;
	Collaborators.Session->ClearCurrentSessionData();
	TestTrue(TEXT("signed out, a spectator has no id"), CrowdySpectator::ViewerUuid(*Collaborators.Session).IsEmpty());
	TestFalse(TEXT("and mints none"), Collaborators.Session->GetID().IsValid());

	Collaborators.Session->SetUserID(PossessionTestUserID);
	const FString Viewer = CrowdySpectator::ViewerUuid(*Collaborators.Session);
	TestEqual(TEXT("signed in, a spectator mints the session's id"), Viewer, Collaborators.Session->GetUUID());
	TestTrue(TEXT("a valid one"), Collaborators.Session->GetID().IsValid());

	ACrowdyPlayerDerivedTestPawn* Pawn = SpawnPlayerDerivedPawn(TestWorld.World, Collaborators);
	APlayerController* Controller = SpawnLocalPlayerController(TestWorld.World);
	if (!TestNotNull(TEXT("the pawn spawned"), Pawn) || !TestNotNull(TEXT("the controller spawned"), Controller))
	{
		return false;
	}
	Pawn->DispatchBeginPlay();
	Pawn->PossessedBy(Controller);
	TestEqual(TEXT("the pawn reuses the spectator's id"), Pawn->Entity->GetNetID().ToString(EGuidFormats::Digits), Viewer);
	TestEqual(TEXT("and registers under it"), Collaborators.Entities->FindEntityID(Pawn), Collaborators.Session->GetID());
	return true;
}

// A respawn that keeps the previous pawn alive after unpossession (DetachFromControllerPendingDestroy plus a
// lifespan): the new pawn reuses the session's id, takes it from the corpse, and keeps it once the corpse ends play.
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

	First->DispatchBeginPlay();
	First->PossessedBy(Controller);
	const FGuid Expected = Collaborators.Session->GetID();
	TestTrue(TEXT("the session holds a valid id"), Expected.IsValid());
	TestEqual(TEXT("the first pawn is registered under the session's id"), Collaborators.Entities->FindEntityID(First), Expected);

	First->UnPossessed();
	Second->DispatchBeginPlay();
	Second->PossessedBy(Controller);

	TestEqual(TEXT("the second pawn reuses the session's id"), Collaborators.Entities->FindEntityID(Second), Expected);
	TestEqual(TEXT("and the session keeps it"), Collaborators.Session->GetID(), Expected);
	TestEqual(TEXT("the registry's participant for the id is the second pawn"),
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
	Departed->Entity->AssignPooledIdentity(Reused, FGuid(), 0, ECrowdyRole::RemoteProxy, CROWDY_INVALID_CLASS_ID);
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
	Registered->Entity->AssignPooledIdentity(Own, FGuid(), 0, ECrowdyRole::RemoteProxy, CROWDY_INVALID_CLASS_ID);
	Registered->DispatchBeginPlay();
	Registered->Entity->DestroyComponent();
	TestNull(TEXT("an actor's own record goes when it ends play"), Collaborators.Entities->FindRecord(Own));
	return true;
}

// A record this session writes reads its owner user live from the session, whatever the record holds; a peer's record
// reads the user that peer claimed; a Leader or World entity has no owner user.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntityLocalRecordReadsSessionUserTest,
	"CrowdySDK.Entity.LocalRecordReadsTheSessionsUser", CrowdyAuthorityTestFlags)
bool FCrowdyEntityLocalRecordReadsSessionUserTest::RunTest(const FString& Parameters)
{
	constexpr int64 StaleClaim = 999;
	constexpr int64 PeerUser = 321;
	constexpr int64 NextUser = 4343;
	const FCrowdyPossessionCollaborators Collaborators;
	const FGuid Local = FGuid::NewGuid();
	Collaborators.Entities->SetLocalPlayerID(Local);
	Collaborators.Entities->SetGameSessionForTest(Collaborators.Session);
	TestEqual(TEXT("the local user is the session's"), Collaborators.Entities->GetLocalUserID(), PossessionTestUserID);

	FCrowdyEntityRecord Mine;
	Mine.NetID = FGuid::NewGuid();
	Mine.WriterID = Local;
	Mine.Role = ECrowdyRole::Owner;
	Mine.OwnerUserID = StaleClaim;
	TestEqual(TEXT("a record written here reads the signed-in user, not what it holds"),
		Collaborators.Entities->GetOwnerUserID(Mine), PossessionTestUserID);

	FCrowdyEntityRecord Theirs = Mine;
	Theirs.WriterID = FGuid::NewGuid();
	Theirs.Role = ECrowdyRole::RemoteProxy;
	Theirs.OwnerUserID = PeerUser;
	TestEqual(TEXT("a peer's record reads the user its writer claimed"), Collaborators.Entities->GetOwnerUserID(Theirs), PeerUser);

	FCrowdyEntityRecord Leader = Mine;
	Leader.bLeaderOwned = true;
	TestEqual(TEXT("a Leader entity has no owner user, even on the Leader"), Collaborators.Entities->GetOwnerUserID(Leader), static_cast<int64>(0));

	FCrowdyEntityRecord World = Theirs;
	World.WriterID = FGuid();
	World.Role = ECrowdyRole::WorldOwned;
	TestEqual(TEXT("nor has a World entity"), Collaborators.Entities->GetOwnerUserID(World), static_cast<int64>(0));

	AActor* Actor = NewObject<AActor>(GetTransientPackage());
	UCrowdyEntityComponent* Component = NewObject<UCrowdyEntityComponent>(Actor);
	Mine.Participant = Actor;
	Collaborators.Entities->RegisterEntity(Mine);
	Component->SetCollaboratorsForTest(Collaborators.Entities, Collaborators.Session);
	Component->AssignPooledIdentity(Mine.NetID, Local, 0, ECrowdyRole::Owner, 0);
	TestEqual(TEXT("the entity's component reads the same user"), Component->GetOwnerUserID(), PossessionTestUserID);
	TestTrue(TEXT("and is owned by the local user"), Component->IsOwnedByLocalUser());

	const TArray<AActor*> MineFound = Collaborators.Entities->GetEntitiesByOwnerUser(PossessionTestUserID);
	TestTrue(TEXT("the signed-in user finds the entity this session writes"), MineFound.Num() == 1 && MineFound[0] == Actor);
	TestEqual(TEXT("and the stale claim its record holds finds nothing"), Collaborators.Entities->GetEntitiesByOwnerUser(StaleClaim).Num(), 0);

	Collaborators.Session->SetUserID(NextUser);
	TestEqual(TEXT("another user's sign-in is read at once"), Component->GetOwnerUserID(), NextUser);

	Collaborators.Session->SetUserID(0);
	TestEqual(TEXT("signed out, the owner user is unknown"), Component->GetOwnerUserID(), static_cast<int64>(0));
	TestFalse(TEXT("and an unknown user owns nothing"), Component->IsOwnedByLocalUser());
	TestTrue(TEXT("while this session still writes it"), Component->IsLocallyOwned());
	return true;
}

// Two sessions of one account: the one that does not write an entity is a viewer of it, though its user owns it. Only
// a grant naming this session's actor id makes it the writer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyEntitySecondSessionIsAViewerTest,
	"CrowdySDK.Entity.SecondSessionOfTheSameUserIsAViewer", CrowdyAuthorityTestFlags)
bool FCrowdyEntitySecondSessionIsAViewerTest::RunTest(const FString& Parameters)
{
	const FCrowdyPossessionCollaborators Collaborators;
	UCrowdyEntitySubsystem* Entities = Collaborators.Entities;
	const FGuid Viewer = FGuid::NewGuid();
	const FGuid Writer = FGuid::NewGuid();
	Entities->SetLocalPlayerID(Viewer);
	Entities->SetGameSessionForTest(Collaborators.Session);

	ACrowdyStateHostOverrideActor* Actor = NewObject<ACrowdyStateHostOverrideActor>();
	if (!TestNotNull(TEXT("actor created"), Actor) || !TestNotNull(TEXT("entity component present"), Actor->Entity.Get()))
	{
		return false;
	}
	Actor->Entity->ReplicateMovement = ECrowdyReplicateMovement::Off;

	FCrowdyEntityRecord Record;
	Record.NetID = FGuid::NewGuid();
	Record.WriterID = Writer;
	Record.Role = ECrowdyRole::RemoteProxy;
	Record.OwnerUserID = PossessionTestUserID;
	Record.Participant = Actor;
	Entities->RegisterEntity(Record);
	Actor->Entity->SetCollaboratorsForTest(Entities, Collaborators.Session);
	Actor->Entity->AssignPooledIdentity(Record.NetID, Writer, PossessionTestUserID, ECrowdyRole::RemoteProxy, 0);

	TestTrue(TEXT("the signed-in user owns the entity"), Actor->Entity->IsOwnedByLocalUser());
	TestFalse(TEXT("but this session does not write it"), Entities->IsLocallyOwned(Record.NetID));
	TestFalse(TEXT("nor does its component say so"), Actor->Entity->IsLocallyOwned());

	const FGuid ThirdSession = FGuid::NewGuid();
	Entities->ReassignOwnership(Record.NetID, ThirdSession, Writer, PossessionTestUserID);
	TestTrue(TEXT("a grant to another session of the same user leaves this one a viewer"),
		Entities->FindRecord(Record.NetID)->Role == ECrowdyRole::RemoteProxy);
	TestFalse(TEXT("that does not write it"), Entities->IsLocallyOwned(Record.NetID));

	Entities->ReassignOwnership(Record.NetID, Viewer, ThirdSession, PossessionTestUserID);
	TestTrue(TEXT("a grant naming this session makes it the writer"), Entities->FindRecord(Record.NetID)->Role == ECrowdyRole::Owner);
	TestTrue(TEXT("which writes it"), Entities->IsLocallyOwned(Record.NetID));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
