// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyAutoReplicator.generated.h"


class ICrowdyReplicationSource;
class UCrowdySDKBridgeSubsystem;

namespace CrowdyAutoReplication
{
	/** What one entry should put on the wire this interval. */
	enum class ESendDecision : uint8
	{
		/** Nothing. The actor is unchanged and neither clock is due. */
		None,
		/** The actor's full state, which is also what a keyframe is. */
		FullUpdate,
		/** The spatial header alone, saying the actor is still here without restating it. */
		Heartbeat,
	};

	struct FSendInputs
	{
		/** Off means the old behaviour: every interval is a full send, whatever else is true. */
		bool bSendOnlyOnChange = true;

		/** False until this entry has sent once, which is what makes its first interval unconditional. */
		bool bHasSentBefore = false;

		/** Either the replicated state or the chunk differs from what was last put on the wire. */
		bool bStateOrChunkChanged = false;

		bool bKeyframeDue = false;
		bool bHeartbeatDue = false;
	};

	/**
	 * The whole cadence rule in one place, with no clock, no world and no send in it, so every combination can be
	 * stated as a test rather than reasoned about from the loop that calls it.
	 */
	CROWDYREPLICATION_API ESendDecision DecideSend(const FSendInputs& Inputs);

	/**
	 * Whether an entry's live state differs from what it last put on the wire. ComparableBytes is the run of
	 * bytes that stands in for the last-sent type's properties, which is what lets the loop answer with a
	 * memcmp; zero routes to the engine's own compare.
	 */
	CROWDYREPLICATION_API bool HasStateChanged(const FInstancedStruct& LastSent, const FInstancedStruct& Live,
		int32 ComparableBytes);

	/**
	 * The compare width an entry should carry after sending SentStruct, given what it carries now. Resolving
	 * the width is a reflection walk, so it must not run on a send that did not change which type is sent.
	 */
	CROWDYREPLICATION_API int32 ResolveCompareBytes(const UScriptStruct* PreviousStruct,
		const UScriptStruct* SentStruct, int32 CurrentBytes);
}

struct FReplicationData
{
	// Hot path: only what's needed per-frame
	TArray<FVector3f> Positions;
	TArray<FInt64Vector> Chunks;
	TArray<FString> UUIDs;
	// Weak pointer guards lifetime; the raw interface pointer (resolved once at
	// registration) avoids a Cast<> per entry per replication tick.
	TArray<TWeakObjectPtr<const UActorComponent>> Components;
	TArray<const ICrowdyReplicationSource*> Sources;

	// The entity class each entry puts on the wire, resolved once at registration for the same reason
	// Sources holds a raw interface pointer: UCrowdyClassRegistry::GetID builds the class's path into an
	// FString and hashes it into an FName, which is a heap allocation per entry per replication tick if
	// the loop asks for it. A registered component's actor cannot change class, so once is enough.
	TArray<FCrowdyClassID> ClassIDs;

	// What was last put on the wire for each entry, which is what makes "has this changed" answerable. The state
	// is held by value because the source owns its own and is free to mutate it in place between ticks, so a
	// reference would compare a thing against itself.
	TArray<FInstancedStruct> LastSentStates;

	// The run of bytes that stands in for LastSentStates[i]'s properties, or zero when none does. Resolved
	// only when an entry's state type changes, so the loop answers "has this changed" with a pointer compare
	// and a memcmp rather than a reflection walk.
	TArray<int32> LastSentCompareBytes;

	TArray<FInt64Vector> LastSentChunks;

	// Seconds, from the world's unpaused time. Staggered at registration so a crowd of actors registered in the
	// same frame does not then send its keyframes in the same frame forever after.
	TArray<double> NextKeyframeTimes;
	TArray<double> NextHeartbeatTimes;

	// False until an entry has sent once, so its first tick is always a full send rather than a comparison
	// against a default-constructed state that happens to match.
	TArray<bool> bHasSent;

	int32 Count = 0;
};

/**
 * 
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyAutoReplicator : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual TStatId GetStatId() const override;
	virtual void Tick(float DeltaTime) override;
	
	// Component must implement ICrowdyReplicationSource
	void RegisterReplicationComponent(UActorComponent* Component);
	void UnregisterReplicationComponent(UActorComponent* Component);
	
private:
	
	UCrowdySDKBridgeSubsystem* Bridge = nullptr;

	FReplicationData Data;
	
	float ReplicationInterval = 0.1f;
	float ReplicationAccumulator = 0.f;

	// Read once from the map profile at initialise, since the profile cannot change under a running world.
	bool bSendOnlyOnChange = true;
	float KeyframeIntervalSeconds = 3.0f;
	float HeartbeatIntervalSeconds = 1.0f;

	bool bIsTicking = false;

private:
	void ReplicationLoop();

	/** Seconds on the world's unpaused clock, which is the same clock the two intervals above are expressed in. */
	double NowSeconds() const;

	/**
	 * Spread an entry's first keyframe and first heartbeat across their intervals rather than starting both at
	 * zero. Without it every actor registered in one frame keeps sending in the same frame as every other, which
	 * turns a steady trickle into a periodic spike for no benefit.
	 */
	void ScheduleFirstSends(int32 Index);
};
