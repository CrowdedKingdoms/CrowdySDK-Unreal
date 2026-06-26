#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Interfaces/ICrowdyReceptionLayer.h"
#include "Data/CrowdyEntityTypes.h"
#include "Engine/StreamableManager.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyEntitySubsystem.generated.h"

class UCrowdyEntityComponent;
class UCrowdyGameSession;
class UCrowdySDKBridgeSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyEntityRegistered, const FGuid&, NetID);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyEntityUnregistered, const FGuid&, NetID);

/**
 * Authoritative FGuid based entity registry for everything SDK replicates.
 * Other systems (actor pool, entity components) register here and delegate
 * their lookups; they no longer keep their own ID maps.
 *
 * Also owns the networked entity lifecycle (spawn/destroy events): it is the
 * reception layer for FCrowdyEntitySpawnEvent/FCrowdyEntityDestroyEvent and
 * routes their callbacks to the entity's UCrowdyEntityComponent. Everything
 * between spawn and destroy travels through UCrowdyEventRouter as ordinary
 * game events.
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Entity Subsystem"))
class CROWDYREPLICATION_API UCrowdyEntitySubsystem : public UTickableWorldSubsystem, public ICrowdyReceptionLayer
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	// Drains entity events queued from the network thread
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdyEntitySubsystem, STATGROUP_Tickables);
	}

	// ICrowdyReceptionLayer
	virtual void OnMessageReceived(TSharedRef<ICrowdyMessage> Message) override;
	virtual TArray<ECrowdyMessageType> GetSupportedResponseTypes() const override;
	virtual TArray<UScriptStruct*> GetSupportedEvents() const override;

	//Registry

	void RegisterEntity(const FCrowdyEntityRecord& Record);
	void UnregisterEntity(const FGuid& NetID);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Subsystem")
	AActor* FindEntity(const FGuid& NetID) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Subsystem")
	FGuid FindEntityID(const AActor* Actor) const;

	const FCrowdyEntityRecord* FindRecord(const FGuid& NetID) const;

	// True when the record's OwnerID matches the local player ID
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Subsystem")
	bool IsLocallyOwned(const FGuid& NetID) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Subsystem")
	FGuid GetLocalPlayerID() const;

	// Reads through to UCrowdyGameSession
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Subsystem")
	FGuid GetHostID() const;

	/** Normally seeded from the game session automatically; exposed for Blueprints that managed it manually. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Subsystem")
	void SetLocalPlayerID(const FGuid& InLocalPlayerID);

	//Networked entity lifecycle
	/**
	 * Spawns EntityClass locally and broadcasts FCrowdyEntitySpawnEvent so every
	 * client spawns the same class. The spawn is deferred so the entity component
	 * knows its identity before BeginPlay.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Subsystem")
	AActor* SpawnEntity(TSubclassOf<AActor> EntityClass, const FTransform& SpawnTransform, const FInstancedStruct& InitialState);

	/** Broadcasts FCrowdyEntityDestroyEvent, then destroys after the component's DestroyDelay. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Subsystem")
	void DestroyEntity(AActor* TargetEntity);

	// Legacy seed-based registration; new code should place a UCrowdyEntityComponent
	// with IdentityPolicy=Stable instead.
	void RegisterStaticEntity(AActor* Entity, bool bUseDeterministicID, int64 Seed);

	/**
	 * Sends a game event stamped with the local player as sender. Targeted sends
	 * address a registered entity actor: Target=Entity routes to that entity's
	 * remote instances, Target=Owner routes to the client that owns it.
	 */
	void DispatchGameEvent(const AActor* Context, FInstancedStruct&& Payload,
		ECrowdyTarget Target = ECrowdyTarget::Everyone, const AActor* TargetEntity = nullptr,
		ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay,
		ECrowdyReplicationDistance ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks);

	/**
	 * Sends a payload to the single client that owns TargetActor (a registered entity), using the
	 * actor-to-actor transport. The server delivers it only to that owner — no spatial broadcast,
	 * no echo to the sender. The chunk is taken from TargetActor's current location.
	 */
	void DispatchSingleActorMessage(const AActor* TargetActor, FInstancedStruct Payload);

	/**
	 * Publishes an already-encoded reliable RPC payload over the named channel (empty = the app-wide
	 * default session channel). It reaches every member of that channel regardless of distance,
	 * bypassing the spatial path's decay and range thinning. The bytes are produced by
	 * FCrowdyRPC::EncodeChannelRpc.
	 */
	void PublishReliableRpc(const FString& ChannelName, const TArray<uint8>& ChannelPayload);

	/** Returns all actors whose entity record matches the given owner UUID. */
	TArray<AActor*> GetEntitiesByOwner(const FGuid& OwnerID) const;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Subsystem|Events")
	FOnCrowdyEntityRegistered OnEntityRegistered;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Subsystem|Events")
	FOnCrowdyEntityUnregistered OnEntityUnregistered;

private:

	// Remote spawn whose class is still streaming in. A destroy event arriving
	// during the load cancels the spawn. Router events targeted at the entity
	// while it loads are dropped — senders put initial data in the spawn payload.
	struct FPendingRemoteSpawn
	{
		FCrowdyEntitySpawnEvent SpawnEvent;
		TSharedPtr<FStreamableHandle> LoadHandle;
	};

	TMap<FGuid, FCrowdyEntityRecord> Records;
	TMap<TObjectKey<AActor>, FGuid> ActorToID;
	TMap<FGuid, FPendingRemoteSpawn> PendingRemoteSpawns;

	// Entity events arrive on the network thread and are dispatched on Tick
	TQueue<FInstancedStruct, EQueueMode::Mpsc> PendingEntityEvents;

	UPROPERTY()
	TObjectPtr<UCrowdyGameSession> GameSession;

	UPROPERTY()
	FGuid LocalPlayerID;

	UCrowdySDKBridgeSubsystem* Bridge = nullptr;

	UFUNCTION()
	void OnOwnerUUIDUpdated(FString NewUUID);

	void HandleRemoteSpawn(const FCrowdyEntitySpawnEvent& Event);
	void HandleRemoteDestroy(const FCrowdyEntityDestroyEvent& Event);

	void OnRemoteSpawnClassLoaded(FGuid EntityID, FSoftClassPath ClassPath);
	void FinishRemoteSpawn(const FCrowdyEntitySpawnEvent& Event, UClass* EntityClass);

	// Guarantees lifecycle callbacks have a home when the class ships without a
	// component. Identity is injected before RegisterComponent so the component's
	// BeginPlay registers the record instead of minting its own ID.
	UCrowdyEntityComponent* AddStaticEntityComponent(AActor* Actor, const FGuid& EntityID,
		const FGuid& EntityOwnerID, ECrowdyRole Role, uint32 ClassID) const;

	void DestroyAfterDelay(AActor* Actor, float Delay) const;
};
