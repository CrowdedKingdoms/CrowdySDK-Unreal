#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Replication/RPC/CrowdyEvent.h"
#include "CrowdyMassDispatchProxyTestTypes.generated.h"

class USkeletalMeshComponent;

/**
 * Stands in for a game's own character class in the dispatch proxy prototype: a native root, a native
 * skeletal mesh, and one event body that reaches for that mesh the way a presentation body does.
 *
 * The body records what it found rather than only doing something, because the question the prototype
 * exists to answer is not whether the call returns but whether what it touched was real.
 */
// Hidden so a fixture never reaches an author's class or event picker.
UCLASS(Hidden)
class ACrowdyMassDispatchProxyTestActor : public AActor
{
	GENERATED_BODY()

public:
	ACrowdyMassDispatchProxyTestActor();

	// A stand-in for an author's own multicast body. Native rather than Blueprint because a test cannot
	// author a graph; what it shares with a Blueprint body is the thing under test, that it runs through
	// ProcessEvent against a self it did not choose.
	UFUNCTION()
	void ProxyBody(int32 InValue);

	/**
	 * The author's own multicast body, declared the way a game declares one. The dispatch tests drive this
	 * rather than ProxyBody above, so what they exercise is the road an inbound event actually takes:
	 * charged, decoded, class-guarded, and run on a stand-in that was never spawned.
	 */
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "SpatialMulticast"))
	void ProxyPresent_Implementation(int32 Value);
	CROWDY_EVENT(ProxyPresent)

	// Written by ProxyPresent, and reflected, so a second entity's dispatch that still saw the first
	// entity's value would say the reset between entities did not happen.
	UPROPERTY()
	int32 CarriedValue = 0;

	// What the body observed, deliberately not reflected: it is evidence for a test rather than state of
	// the actor, and reflecting it would put it in the property block the reset copies over.
	int32 PresentRunCount = 0;
	int32 PresentSawCarriedValueOnEntry = 0;
	FVector PresentSawLocation = FVector::ZeroVector;

	// Written by the body, and reset from the class default object, so a reset that skipped it would show
	// up as this holding the previous entity's value.
	UPROPERTY()
	int32 LastValue = 0;

	UPROPERTY()
	TObjectPtr<USceneComponent> ProxyRoot;

	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> ProxyMesh;

	// Deliberately not reflected: what the body observed is evidence for the test, not state of the actor,
	// and reflecting it would put it in the property block the reset copies over.
	int32 BodyRunCount = 0;
	bool bBodySawWorld = false;
	bool bBodySawMesh = false;
	bool bBodySawRegisteredMesh = false;
	bool bBodySawAnimInstance = false;
	FVector BodySawLocation = FVector::ZeroVector;
};
