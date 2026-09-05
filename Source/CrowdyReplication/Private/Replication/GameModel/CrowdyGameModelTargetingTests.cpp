#include "Replication/GameModel/CrowdyGameModelSubsystem.h"
#include "Replication/GameModel/CrowdyGameModelTestTarget.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Replication/Components/CrowdyEntityComponent.h" // ECrowdyOwnership
#include "Replication/Subsystems/CrowdyEntitySubsystem.h"

// Targeting + auto-bind create-authority (N1b). These cover the universal target-resolution seam every Blueprint
// entry point now shares (any registered participant, actor or subsystem, resolves to its NetID), and the pure
// create-authority gate that lets a Host-owned container (a tagged subsystem) be created only by the elected host.
namespace
{
	constexpr EAutomationTestFlags CrowdyTargetingTestFlags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter;

	// Distinct helper names (not the CrowdyGameModelApplyTests / ChangeEvent ones) so the test TUs never collide in
	// a unity build.
	UCrowdyGameModelSubsystem* MakeTargetingModel()
	{
		return NewObject<UCrowdyGameModelSubsystem>(GetTransientPackage());
	}

	UCrowdyEntitySubsystem* MakeTargetingEntities(const FGuid& LocalPlayer)
	{
		UCrowdyEntitySubsystem* Entities = NewObject<UCrowdyEntitySubsystem>(GetTransientPackage());
		Entities->SetLocalPlayerID(LocalPlayer);
		return Entities;
	}
}

// The universal target-resolution seam resolves ANY registered participant to its NetID, not just actors: a plain
// UObject enrolled via RegisterParticipant (the subsystem case) round-trips through ResolveTargetNetID and
// ResolveEntityParticipant, while a null or unregistered object resolves to nothing. This is the read/target
// foundation the widened UObject* getters and the Apply Crowdy Effect Target pin share.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCrowdyGameModelResolveTargetTest,
	"CrowdySDK.GameModel.ResolveTargetNetID", CrowdyTargetingTestFlags)
bool FCrowdyGameModelResolveTargetTest::RunTest(const FString& Parameters)
{
	const FGuid LocalPlayer = FGuid::NewGuid();
	UCrowdyGameModelSubsystem* Model = MakeTargetingModel();
	UCrowdyEntitySubsystem* Entities = MakeTargetingEntities(LocalPlayer);
	UCrowdyGameModelTestTarget* Participant = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	if (!TestNotNull(TEXT("subsystem"), Model) || !TestNotNull(TEXT("entities"), Entities)
		|| !TestNotNull(TEXT("participant"), Participant))
	{
		return false;
	}

	Model->SetEntitySubsystemForTest(Entities);

	// A non-actor UObject enrolled as a participant resolves to its NetID through the single seam.
	const FGuid NetID = Entities->RegisterParticipant(Participant, ECrowdyOwnership::Host);
	TestTrue(TEXT("participant got a valid NetID"), NetID.IsValid());

	FGuid Resolved;
	TestTrue(TEXT("ResolveTargetNetID resolves a non-actor participant"),
		Model->ResolveTargetNetID(Participant, Resolved));
	TestEqual(TEXT("resolved NetID matches the enrolled one"), Resolved, NetID);

	// The reverse participant lookup round-trips to the same object (via the public entity API).
	TestTrue(TEXT("the enrolled NetID resolves back to the participant"),
		Entities->FindParticipant(NetID) == Participant);

	// A null target resolves to nothing and leaves the out NetID invalid.
	FGuid NullResolved;
	TestFalse(TEXT("a null target does not resolve"), Model->ResolveTargetNetID(nullptr, NullResolved));
	TestFalse(TEXT("out NetID is invalid for a null target"), NullResolved.IsValid());

	// An unregistered object resolves to nothing.
	UCrowdyGameModelTestTarget* Unregistered = NewObject<UCrowdyGameModelTestTarget>(GetTransientPackage());
	FGuid UnregResolved;
	TestFalse(TEXT("an unregistered object does not resolve"), Model->ResolveTargetNetID(Unregistered, UnregResolved));

	// With no entity subsystem, resolution fails closed rather than crashing.
	Model->SetEntitySubsystemForTest(nullptr);
	FGuid NoEntities;
	TestFalse(TEXT("resolution fails closed with no entity subsystem"),
		Model->ResolveTargetNetID(Participant, NoEntities));

	return true;
}

#endif
