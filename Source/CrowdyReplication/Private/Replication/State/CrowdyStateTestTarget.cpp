#include "Replication/State/CrowdyStateTestTarget.h"

#include "Replication/Components/CrowdyEntityComponent.h"

// Phase 0 reads only the OnRep metadata value (the function name); the notify is not invoked until
// the receive path (Phase 4). The body exists so the reflected UFUNCTION links.
void UCrowdyStateTestTarget::OnRep_Health()
{
}

ACrowdyStateOverlapActor::ACrowdyStateOverlapActor()
{
	// Entity component and its executor are default subobjects so both are visible on the CDO:
	// FindComponentByClass sees the constructor-created component (as HasEntityComponent relies on), and
	// StateExecutor carries the executor whose GetStateStruct the overlap resolver reads. Assigning
	// StateExecutor in the constructor works regardless of the component's Mode EditCondition, which gates
	// only editor visibility, not the C++ value.
	Entity = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("Entity"));
	Entity->StateExecutor = CreateDefaultSubobject<UCrowdyStateOverlapExecutor>(TEXT("StateExecutor"));
}

ACrowdyStateHostOverrideActor::ACrowdyStateHostOverrideActor()
{
	// Static mode: no continuous channel; the entity component exists only so the HostOverride policy is
	// readable off the actor via FindComponentByClass on both the send and receive paths.
	Entity = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("Entity"));
	Entity->Mode = ECrowdyEntityMode::Static;
}

ACrowdyStateHeartbeatActor::ACrowdyStateHeartbeatActor()
{
	// Static mode: the component exists only so a test can set StateHeartbeat, which the replicator reads via
	// FindComponentByClass in BuildOwnedState. A test overrides StateHeartbeat on the instance before registering.
	Entity = CreateDefaultSubobject<UCrowdyEntityComponent>(TEXT("Entity"));
	Entity->Mode = ECrowdyEntityMode::Static;
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
