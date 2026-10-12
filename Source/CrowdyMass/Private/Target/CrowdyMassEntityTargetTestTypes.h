#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Replication/RPC/CrowdyEvent.h"
#include "CrowdyMassEntityTargetTestTypes.generated.h"

/**
 * Declares the event this fixture family exists to check the class of: a receiver on a C++ base class,
 * so CrowdyMassTarget::CheckEventClass's base-class acceptance rule (a recorded class that DERIVES the
 * declaring class, not just equals it) has a real base/derived pair to run against.
 */
UCLASS()
class UCrowdyMassEventClassCheckBaseTestTarget : public UObject
{
	GENERATED_BODY()

public:
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "OwningClient"))
	void CheckEvent_Implementation(int32 InValue);
	CROWDY_EVENT(CheckEvent)
};

// A Blueprint-derived class would ordinarily supply this half of the pair; a plain C++ subclass with no
// added members is the smallest stand-in that still derives the base rather than equals it.
UCLASS()
class UCrowdyMassEventClassCheckDerivedTestTarget : public UCrowdyMassEventClassCheckBaseTestTarget
{
	GENERATED_BODY()
};

// A class that does not derive the receiver's declaring class at all, for the genuine-mismatch case.
UCLASS()
class UCrowdyMassEventClassCheckUnrelatedTestTarget : public UObject
{
	GENERATED_BODY()
};

// A component declaring an event. An entity class never mentions the components it may carry, so the
// class check must not refuse this: the receiving side resolves the body onto the component.
UCLASS()
class UCrowdyMassEventClassCheckTestComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "SpatialMulticast"))
	void ComponentEvent_Implementation(int32 InValue);
	CROWDY_EVENT(ComponentEvent)
};

/**
 * An actor carrying one receiver per CrowdyRecipient value plus one signature mixing four parameter kinds,
 * used by the byte-parity test and by the routing-metadata tests. It stands in for "a live proxy-actor
 * instance" without needing to be spawned: FCrowdyRPC::MarshalCall never reads through the instance for a
 * signature with no object-reference parameter, so the class only needs to exist for reflection.
 */
// Hidden so a fixture never reaches an author's class or event picker.
UCLASS(Hidden)
class ACrowdyMassEventParityTestTarget : public AActor
{
	GENERATED_BODY()

public:
	// int32 + float + FString + FVector: four parameter kinds, so a parity check over this signature
	// cannot pass by accident the way a single-int signature could.
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "OwningClient"))
	void ParityEvent_Implementation(int32 InAmmo, float InSpeed, const FString& InLabel, FVector InDir);
	CROWDY_EVENT(ParityEvent)

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "Host"))
	void HostRecipientEvent_Implementation(int32 InValue);
	CROWDY_EVENT(HostRecipientEvent)

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "Multicast"))
	void MulticastRecipientEvent_Implementation(int32 InValue);
	CROWDY_EVENT(MulticastRecipientEvent)

	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "SpatialMulticast"))
	void SpatialRecipientEvent_Implementation(int32 InValue);
	CROWDY_EVENT(SpatialRecipientEvent)

	// Deliberately carries no CrowdyEvent metadata, so UCrowdyBakedRegistry::IsEventHandlerFunction refuses
	// it before anything else runs.
	UFUNCTION()
	void NotAnEvent_Implementation(int32 InValue);
};
