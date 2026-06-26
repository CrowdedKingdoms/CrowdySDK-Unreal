#pragma once

#include "CoreMinimal.h"
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

	TWeakObjectPtr<AActor> Actor;
};
