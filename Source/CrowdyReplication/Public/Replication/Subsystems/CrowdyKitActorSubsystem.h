#pragma once

#include "CoreMinimal.h"
#include "Messages/Actor/FCrowdyKitActorState.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyKitActorSubsystem.generated.h"

class UCrowdyActorTracker;
class UCrowdyEntitySubsystem;
class UCrowdyGameSession;
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

	/** The kit actors of Kind this player sees now, such as every mob; every kind when Kind is None. Copies them all: to read them every frame, use Get Kit Actor Count and Get Kit Actor. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Kit")
	TArray<FCrowdyKitActorState> GetKitActors(FName Kind) const;

	/** How many kit actors of Kind this player sees now; every kind when Kind is None. Cheap enough to call every frame, then read each with Get Kit Actor. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Kit")
	int32 GetKitActorCount(FName Kind) const;

	/** The kit actor of Kind at Index, from 0 up to Get Kit Actor Count; every kind when Kind is None. False, and an empty kit actor, when Index is out of range. A kit actor's Index changes as others come and go, so keep its Id to follow it. */
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Kit")
	bool GetKitActor(FName Kind, int32 Index, FCrowdyKitActorState& KitActor) const;

	/** The kit actors of Kind in place, none for a kind not seen. A Kind of None is the actors of no named kind. Valid until the next update is taken, so read it at once. */
	const TArray<FCrowdyKitActorState>& ViewKitActors(FName Kind) const;

	/** The kit actor the SDK knows by Key, such as a Crowdy Kit Actor component's; null when it is not seen now. */
	const FCrowdyKitActorState* FindKitActor(const FGuid& Key) const;

	/** Unreal units per chunk where kit actors are placed: the session's, or the platform's where there is none. */
	double GetChunkSize() const;

	/** Where State's chunks put it in this world, in units. */
	FVector PlaceInWorld(const FCrowdyKitActorState& State) const { return State.ChunkPosition * GetChunkSize(); }

#if WITH_DEV_AUTOMATION_TESTS
	void UseSessionForTest(const UCrowdyGameSession* InSession);
#endif

	// Takes a batch the tracker handed over on the game thread: what changed for each kit actor reaches its component.
	void TakeUpdates(const TArray<FCrowdyActorUpdate>& Updates);
	void Forget(const FGuid& Key);

private:
	struct FKitSlot
	{
		FName Kind;
		int32 Index = 0;
	};

	// Everything of one kind, with the key of each state at the same index.
	struct FKindActors
	{
		TArray<FCrowdyKitActorState> States;
		TArray<FGuid> Keys;
	};

	UFUNCTION()
	void HandleAppeared(FGuid UUID, FInstancedStruct InitialState, int32 ActorCount);

	UFUNCTION()
	void HandleTimedOut(FGuid UUID, int32 ActorCount);

	UFUNCTION()
	void HandleLeft(const FCrowdyActorLeft& ActorLeft, int32 ActorCount);

	// Runs whenever an actor is drawn for an entity, as when a pooled actor is reused for another kit actor.
	UFUNCTION()
	void HandleEntityRegistered(const FGuid& NetID);

	void ResolveSession();

	// Keeps State under Key with its Location placed by ChunkSize, and returns what is kept.
	const FCrowdyKitActorState& Store(const FGuid& Key, const FCrowdyKitActorState& State, double ChunkSize);

	void Take(const FGuid& Key, const FCrowdyKitActorState& State, double ChunkSize);

	// Does nothing while the actor is not drawn yet, or never is, as on a Mass map.
	void Deliver(const FGuid& Key, const FCrowdyKitActorState& State, bool bStartsDrawing) const;

	TMap<FGuid, FKitSlot> Slots;
	TMap<FName, FKindActors> ByKind;

	TWeakObjectPtr<UCrowdyActorTracker> Tracker;
	TWeakObjectPtr<UCrowdyEntitySubsystem> Entities;
	TWeakObjectPtr<const UCrowdyGameSession> Session;
	FDelegateHandle UpdatesHandle;
};
