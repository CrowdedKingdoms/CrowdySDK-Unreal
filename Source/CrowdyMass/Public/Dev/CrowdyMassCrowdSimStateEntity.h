#pragma once

#include "CoreMinimal.h"
#include "Dev/CrowdyMassGateStateEntity.h"
#include "GameFramework/Actor.h"
#include "CrowdyMassCrowdSimStateEntity.generated.h"

class UCrowdyEntityComponent;

/**
 * Dev-only, near-empty CrowdyState participant. The crowd simulation harness spawns one per simulated
 * player so the SDK's real CrowdyState replicator has something to walk for it: a UCrowdyEntityComponent
 * plus two CrowdyState properties, no mesh, no collision, no tick of its own. This is a different channel
 * from the harness's chunk-based transform updates (UCrowdySDKSubsystem::DispatchActorUpdate); it exists
 * to put the class-diff, typed-property state plane on the wire at crowd scale, which nothing else in the
 * harness could reach without an actual replicated participant to diff against.
 *
 * This is the plugin's own default simulated-player class. UCrowdyMassCrowdSimHarness::Start accepts any
 * Actor class carrying a UCrowdyEntityComponent in its place; a class with no Payload or Stance property
 * of its own simply carries less CrowdyState traffic this run.
 */
// Hidden so a dev-only entity and its events stay out of an author's class and event pickers.
UCLASS(Hidden)
class CROWDYMASS_API ACrowdyMassCrowdSimStateEntity : public AActor
{
	GENERATED_BODY()

public:
	ACrowdyMassCrowdSimStateEntity();

	// A synthetic payload the harness mutates at a configurable per-bot rate to produce on-change CrowdyState
	// deltas. Marked CrowdyHeartbeat so it also rides the periodic keyframe re-send: at crowd scale that
	// standing cost, not the on-change traffic, is usually what matters, and a fixture with no heartbeat
	// property could never put it on the wire at all.
	UPROPERTY(meta = (CrowdyState, CrowdyHeartbeat))
	int32 Payload = 0;

	// Declared as the plugin's own gate stance enum so a crowd observer can bind a pose writer to it: the
	// type is what a receive-side ingestor matches against, the same way ECrowdyMassGateStance is matched
	// elsewhere in this module. For the pose to be SEEN rather than merely delivered, the class spawned as
	// a simulated player needs an animation set of its own naming that enum; without one the delta still
	// arrives and still exercises the plane, but nothing draws it.
	UPROPERTY(meta = (CrowdyState, CrowdyHeartbeat))
	ECrowdyMassGateStance Stance = ECrowdyMassGateStance::Idle;

	UCrowdyEntityComponent* GetEntityComponent() const { return EntityComp; }

private:
	UPROPERTY()
	TObjectPtr<UCrowdyEntityComponent> EntityComp;
};
