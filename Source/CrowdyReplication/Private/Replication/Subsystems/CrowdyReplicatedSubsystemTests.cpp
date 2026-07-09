// Subsystem Replication Phase 4 (ergonomics). Exercises the enrollment helpers the ergonomic bases funnel
// through (UCrowdyReplicatedSubsystemLibrary) via the injectable *Into core, so no world is needed: a
// deterministic-identity round-trip through RegisterReplicatedSubsystemInto / UnregisterReplicatedSubsystemInto
// and the null-guard failure paths. The world-resolving wrappers and the two UCLASS(Abstract) bases are
// lifecycle glue over this same core; their per-world behavior is the 2-client PIE acceptance gate.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
// ECrowdyOwnership is only forward-declared in CrowdyEntitySubsystem.h; the complete enum lives here. Omitting
// this is the exact Phase 2 build break (an incomplete-enum use), so include it explicitly.
#include "Replication/Components/CrowdyEntityComponent.h"
#include "Replication/State/CrowdyStateTestTarget.h"
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"
#include "Replication/Subsystems/CrowdyReplicatedSubsystemLibrary.h"

namespace
{
	constexpr EAutomationTestFlags CrowdyReplicatedSubsystemTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// A bare entity subsystem (UWorldSubsystem, no ClassWithin) with a local player id set, so
	// RegisterParticipant / FindEntityID / FindParticipant resolve headlessly. The ClassWithin=UGameInstance
	// trap applies only to UGameInstanceSubsystem-derived classes, which this test does not construct.
	UCrowdyEntitySubsystem* MakeEntitySubsystem(const FGuid& LocalPlayer)
	{
		UCrowdyEntitySubsystem* ES = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
		ES->SetLocalPlayerID(LocalPlayer);
		return ES;
	}
}

// Enroll a host-owned participant through the library core: it mints a valid deterministic NetID and the
// registry resolves the participant both ways; unenroll removes it cleanly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySubsystemEnrollmentRoundTripsTest,
	"CrowdySDK.State.SubsystemEnrollmentRoundTrips", CrowdyReplicatedSubsystemTestFlags)
bool FCrowdySubsystemEnrollmentRoundTripsTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(FGuid::NewGuid());
	if (!TestNotNull(TEXT("entity subsystem created"), ES))
	{
		return false;
	}

	UObject* Sub = NewObject<UCrowdyStateSubsystemTestTarget>();
	if (!TestNotNull(TEXT("participant created"), Sub))
	{
		return false;
	}

	const FGuid Id = UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystemInto(ES, Sub, ECrowdyOwnership::Host);
	TestTrue(TEXT("enroll returns a valid deterministic NetID"), Id.IsValid());
	TestEqual(TEXT("FindEntityID round-trips to the enrolled NetID"), ES->FindEntityID(Sub), Id);
	TestTrue(TEXT("FindParticipant round-trips to the enrolled participant"), ES->FindParticipant(Id) == Sub);

	UCrowdyReplicatedSubsystemLibrary::UnregisterReplicatedSubsystemInto(ES, Sub);
	TestFalse(TEXT("FindEntityID is invalid after unenroll"), ES->FindEntityID(Sub).IsValid());
	TestTrue(TEXT("FindParticipant is null after unenroll"), ES->FindParticipant(Id) == nullptr);
	return true;
}

// Two independent clients enrolling the SAME host-owned subsystem class compute the SAME NetID (class-path
// seed, no owner salt), so a host correction addresses one agreed id on every client.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySubsystemEnrollmentDeterministicIdentityTest,
	"CrowdySDK.State.SubsystemEnrollmentDeterministicIdentity", CrowdyReplicatedSubsystemTestFlags)
bool FCrowdySubsystemEnrollmentDeterministicIdentityTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* ES1 = MakeEntitySubsystem(FGuid::NewGuid());
	UCrowdyEntitySubsystem* ES2 = MakeEntitySubsystem(FGuid::NewGuid());
	if (!TestNotNull(TEXT("first entity subsystem created"), ES1) || !TestNotNull(TEXT("second entity subsystem created"), ES2))
	{
		return false;
	}

	UObject* Sub1 = NewObject<UCrowdyStateSubsystemTestTarget>();
	UObject* Sub2 = NewObject<UCrowdyStateSubsystemTestTarget>();

	const FGuid Id1 = UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystemInto(ES1, Sub1, ECrowdyOwnership::Host);
	const FGuid Id2 = UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystemInto(ES2, Sub2, ECrowdyOwnership::Host);

	TestTrue(TEXT("first mint is valid"), Id1.IsValid());
	TestTrue(TEXT("second mint is valid"), Id2.IsValid());
	TestEqual(TEXT("same class + Host ownership -> same deterministic NetID on both clients"), Id1, Id2);
	return true;
}

// A null registry or null participant fails closed: an invalid NetID and no crash, on both enroll and unenroll.
// The library logs these at Warning, which does not fail an automation test, so no AddExpectedError is needed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdySubsystemEnrollmentNullGuardsTest,
	"CrowdySDK.State.SubsystemEnrollmentNullGuards", CrowdyReplicatedSubsystemTestFlags)
bool FCrowdySubsystemEnrollmentNullGuardsTest::RunTest(const FString& Parameters)
{
	UCrowdyEntitySubsystem* ES = MakeEntitySubsystem(FGuid::NewGuid());
	if (!TestNotNull(TEXT("entity subsystem created"), ES))
	{
		return false;
	}

	UObject* Sub = NewObject<UCrowdyStateSubsystemTestTarget>();

	const FGuid NullRegistryId =
		UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystemInto(nullptr, Sub, ECrowdyOwnership::Host);
	TestFalse(TEXT("a null registry returns an invalid NetID"), NullRegistryId.IsValid());

	const FGuid NullSubId =
		UCrowdyReplicatedSubsystemLibrary::RegisterReplicatedSubsystemInto(ES, nullptr, ECrowdyOwnership::Host);
	TestFalse(TEXT("a null participant returns an invalid NetID"), NullSubId.IsValid());

	// Unenroll with null args must also be a no-op, not a crash.
	UCrowdyReplicatedSubsystemLibrary::UnregisterReplicatedSubsystemInto(nullptr, Sub);
	UCrowdyReplicatedSubsystemLibrary::UnregisterReplicatedSubsystemInto(ES, nullptr);

	// Nothing was ever enrolled, so the registry has no record for this participant.
	TestFalse(TEXT("an unenrolled participant has no NetID"), ES->FindEntityID(Sub).IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
