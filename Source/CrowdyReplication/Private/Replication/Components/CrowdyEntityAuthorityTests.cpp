#include "Replication/Components/CrowdyEntityComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Data/CrowdyEntityTypes.h"

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

// The new authoring fields default to the super-user model: LocalClient ownership, Allow host override.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyStateAuthorityDefaultsTest,
	"CrowdySDK.State.AuthorityDefaults", CrowdyAuthorityTestFlags)
bool FCrowdyStateAuthorityDefaultsTest::RunTest(const FString& Parameters)
{
	const UCrowdyEntityComponent* CDO = GetDefault<UCrowdyEntityComponent>();
	if (!TestNotNull(TEXT("component CDO"), CDO))
	{
		return false;
	}
	TestEqual(TEXT("Ownership defaults to LocalClient"),
		static_cast<uint8>(CDO->GetOwnership()), static_cast<uint8>(ECrowdyOwnership::LocalClient));
	TestEqual(TEXT("HostOverride defaults to Allow"),
		static_cast<uint8>(CDO->GetHostOverridePolicy()), static_cast<uint8>(ECrowdyHostOverride::Allow));
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

	// A player owns an entity it spawned (target stamped with the player's id) — the unchanged base case.
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

	// A host-owned entity, as the owner, acts as the host — so it owns what the host owns (e.g. host-spawned actors).
	TestTrue(TEXT("host-owned entity owns a host-spawned entity"),
		UEC::DoesOwnershipMatch(ECrowdyRole::HostOwned, WorldEntity, ECrowdyRole::Owner, HostPlayer, HostPlayer));

	// Host-owned entities all resolve to the same host id, so they mutually "own" each other. Broad by design — the
	// model has no per-world-entity owner id — and documented as such.
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
