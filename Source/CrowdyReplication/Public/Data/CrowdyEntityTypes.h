#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CrowdyEntityTypes.generated.h"

UENUM(BlueprintType)
enum class ECrowdyRole : uint8
{
	None UMETA(DisplayName="None"),        // not registered
	Owner UMETA(DisplayName="Owner"),       // this client simulates and sends (like ROLE_Authority for this entity)
	RemoteProxy UMETA(DisplayName="Remote Proxy"), // receives state/events from the network
	HostOwned UMETA(DisplayName="Host Owned")   // owned by whoever is host (AI/world entities); reassigned on host migration
};

USTRUCT(BlueprintType)
struct FCrowdyEntityRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Entity")
	FGuid NetID;

	// Guid::Zero for world-static entities. 
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Entity")
	FGuid OwnerID;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Entity")
	ECrowdyRole Role = ECrowdyRole::None;

	// Hash-derived class identity carried on spawn events.
	// Not Blueprint-exposed (uint32).
	UPROPERTY()
	uint32 ClassID = 0;

	// Any UObject can be a replicated participant (an actor, or a non-actor UObject such as a subsystem in
	// later phases). Non-UPROPERTY, exactly as the actor weak pointer it replaces. For an actor participant,
	// GetActor() resolves it byte-identically.
	TWeakObjectPtr<UObject> Participant;

	AActor* GetActor() const { return Cast<AActor>(Participant.Get()); }
	UObject* GetParticipant() const { return Participant.Get(); }
};
