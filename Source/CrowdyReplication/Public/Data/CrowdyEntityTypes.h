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
	WorldOwned UMETA(DisplayName="World Owned")   // no owner: a World entity, which no client writes, or a replicated subsystem, which the Leader writes
};

USTRUCT(BlueprintType)
struct FCrowdyEntityRecord
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Entity")
	FGuid NetID;

	// Guid::Zero for world-static entities.
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Entity")
	FGuid WriterID;

	// The owner's user id as a peer claimed it; 0 when unknown. A record written here reads the session's user instead.
	UPROPERTY()
	int64 OwnerUserID = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Entity")
	ECrowdyRole Role = ECrowdyRole::None;

	// Owned By Leader: WriterID follows the Leader, and is empty while there is none.
	bool bLeaderOwned = false;

	// A player's avatar: its NetID is its writing session's id, which a peer's record may not name as WriterID yet.
	bool bAvatar = false;

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

	// A live participant that is not an actor: a replicated subsystem, whose World writer is the Leader.
	bool IsReplicatedSubsystem() const
	{
		const UObject* Object = Participant.Get();
		return Object && !Object->IsA<AActor>();
	}

	// The one answer to "does this client write it": it owns the entity, or the entity has no owner and this client is
	// its writer (the Leader, for a replicated subsystem). An Owner record with no owner id was registered here before
	// the local player id was known, so it is still this client's; one naming another player is not, whatever its role says.
	static bool IsWrittenBy(const ECrowdyRole InRole, const FGuid& InRecordWriterID, const FGuid& PlayerID, const FGuid& AppWriterID)
	{
		if (InRole == ECrowdyRole::Owner)
			return !InRecordWriterID.IsValid() || InRecordWriterID == PlayerID;
		return InRole == ECrowdyRole::WorldOwned && PlayerID.IsValid() && AppWriterID == PlayerID;
	}

	// A label, never a write decision: an unknown user (0) matches nobody, a signed-out local user included.
	static bool IsOwnedByUser(const int64 InOwnerUserID, const int64 LocalUserID)
	{
		return LocalUserID != 0 && InOwnerUserID == LocalUserID;
	}
};
