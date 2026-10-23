#pragma once

#include "CoreMinimal.h"
#include "Messages/Actor/FCrowdyKitActorState.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyKitActorSubsystem.generated.h"

class UCrowdyActorTracker;
class UCrowdyEntitySubsystem;
struct FCrowdyActorLeft;
struct FCrowdyActorUpdate;

/**
 * The Game Kit starters' actors this player sees (mobs, NPCs, pets, a ball), as their hubs last showed them, and the Crowdy Kit Actor
 * component of the actor each is drawn as. On a map drawn with Mass, which has no actor per entity, Get Kit Actors reads them.
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyKitActorSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override
	{
		return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
	}

public:

	/** The kit actors of Kind this player sees now, such as every mob; every kind when Kind is None. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Kit")
	TArray<FCrowdyKitActorState> GetKitActors(FName Kind) const;

	/** The kit actor the SDK knows by Key, such as a Crowdy Kit Actor component's; null when it is not seen now. */
	const FCrowdyKitActorState* FindKitActor(const FGuid& Key) const { return Seen.Find(Key); }

	// Takes a batch the tracker handed over on the game thread: what changed for each kit actor reaches its component.
	void TakeUpdates(const TArray<FCrowdyActorUpdate>& Updates);
	void Forget(const FGuid& Key);

private:
	UFUNCTION()
	void HandleAppeared(FGuid UUID, FInstancedStruct InitialState, int32 ActorCount);

	UFUNCTION()
	void HandleTimedOut(FGuid UUID, int32 ActorCount);

	UFUNCTION()
	void HandleLeft(const FCrowdyActorLeft& ActorLeft, int32 ActorCount);

	// Runs whenever an actor is drawn for an entity, as when a pooled actor is reused for another kit actor.
	UFUNCTION()
	void HandleEntityRegistered(const FGuid& NetID);

	void Take(const FGuid& Key, const FCrowdyKitActorState& State);

	// Does nothing while the actor is not drawn yet, or never is, as on a Mass map.
	void Deliver(const FGuid& Key, const FCrowdyKitActorState& State, bool bStartsDrawing) const;

	TMap<FGuid, FCrowdyKitActorState> Seen;

	TWeakObjectPtr<UCrowdyActorTracker> Tracker;
	TWeakObjectPtr<UCrowdyEntitySubsystem> Entities;
	FDelegateHandle UpdatesHandle;
};
