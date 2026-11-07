#include "Replication/State/CrowdyStateTestTarget.h"

#include "Components/SceneComponent.h"
#include "Replication/Components/CrowdyEntityComponent.h"

// Discovery reads only the OnRep metadata value (the function name); the notify is not invoked until
// the receive path. The body exists so the reflected UFUNCTION links.
void UCrowdyStateTestTarget::OnRep_Health()
{
}

// Writes Wire always and Conditional only while Wire is positive, symmetrically on read and write, so a
// delta carrying a non-positive Wire says nothing about Conditional at all.
bool FCrowdyStatePartialNetStruct::NetSerialize(FArchive& Ar, UPackageMap* /*Map*/, bool& bOutSuccess)
{
	bOutSuccess = true;
	Ar << Wire;
	if (Wire > 0)
	{
		Ar << Conditional;
	}
	return true;
}

ACrowdyStateOverlapActor::ACrowdyStateOverlapActor()
{
	// Entity component and its executor are default subobjects so both are visible on the CDO:
	// FindComponentByClass sees the constructor-created component (as HasEntityComponent relies on), and
	// StateExecutor carries the executor whose GetStateStruct the overlap resolver reads. Assigning
	// StateExecutor in the constructor works regardless of the field's EditCondition, which gates only editor
	// visibility, not the C++ value. Sending movement is required rather than incidental: only such a component
	// runs the continuous channel that writes the executor state struct, so the overlap filter this fixture
	// exercises does nothing without it. It sets the deprecated Mode because the save mapping tests need a
	// component a C++ constructor configured the old way.
	Entity = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("Entity"));
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Entity->Mode = ECrowdyEntityMode::Dynamic;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	Entity->StateExecutor = CreateDefaultSubobject<UCrowdyStateOverlapExecutor>(TEXT("StateExecutor"));
}

ACrowdyStateHostOverrideActor::ACrowdyStateHostOverrideActor()
{
	// No continuous channel; the entity component exists only so the HostOverride policy is readable off the
	// actor via FindComponentByClass on both the send and receive paths.
	Entity = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("Entity"));

	// A root, so the fixture has a place: the spawn cases read where it stands.
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
}

ACrowdyLeaderOwnedTestActor::ACrowdyLeaderOwnedTestActor()
{
	Entity->OwnedBy = ECrowdyOwnedBy::Leader;
}

ACrowdyLeaderMovingTestActor::ACrowdyLeaderMovingTestActor()
{
	Entity->ReplicateMovement = ECrowdyReplicateMovement::On;
}

ACrowdyPlayerDerivedTestPawn::ACrowdyPlayerDerivedTestPawn()
{
	Entity = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("Entity"));
	Entity->IdentityPolicy = ECrowdyIdentityPolicy::PlayerDerived;
	Entity->OwnedBy = ECrowdyOwnedBy::Player;
	AutoPossessAI = EAutoPossessAI::Disabled;
	AutoPossessPlayer = EAutoReceiveInput::Disabled;
}

ACrowdyStateHeartbeatActor::ACrowdyStateHeartbeatActor()
{
	// The component exists only so a test can set StateHeartbeat, which the replicator reads via
	// FindComponentByClass in BuildOwnedState. A test overrides StateHeartbeat on the instance before registering.
	Entity = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("Entity"));
}

ACrowdyStateApplyTestActor::ACrowdyStateApplyTestActor()
{
	// Both components are default subobjects so ResolveStateContainer has a matching and a non-matching
	// component to choose between when the layout owner class is a component.
	ApplyComp = CreateDefaultSubobject<UCrowdyStateApplyTestComponent>(TEXT("ApplyComp"));
	OtherComp = CreateDefaultSubobject<UCrowdyStateApplyOtherComponent>(TEXT("OtherComp"));
}

void ACrowdyStateApplyTestActor::OnRep_Health()
{
	++HealthOnRepCount;
}

void ACrowdyStateApplyTestActor::OnRep_Score()
{
	++ScoreOnRepCount;
}

void ACrowdyStateReentrantNotifyActor::OnRep_First()
{
	++FirstOnRepCount;
	if (NotifyHook)
	{
		NotifyHook();
	}
}

void ACrowdyStateReentrantNotifyActor::OnRep_Third()
{
	++ThirdOnRepCount;
}

void UCrowdyStateSubsystemTestTarget::OnRep_Notified()
{
	++NotifiedOnRepCount;
}

// Bodies exist so the reflected UFUNCTIONs link; the reject fixture only asserts which binding survives
// discovery, so OnRep_Bad is never actually invoked (it takes a parameter, which the validator rejects).
void UCrowdyStateBadOnRepTarget::OnRep_Bad(int32 /*NewValue*/)
{
}

void UCrowdyStateBadOnRepTarget::OnRep_Good()
{
}
