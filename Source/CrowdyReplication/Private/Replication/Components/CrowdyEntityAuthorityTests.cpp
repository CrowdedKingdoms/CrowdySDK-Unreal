#include "Replication/Components/CrowdyEntityComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Data/CrowdyEntityTypes.h"
#include "GameFramework/Actor.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyAuthorityTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;
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

#endif // WITH_DEV_AUTOMATION_TESTS
