#pragma once

#include "CoreMinimal.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "Data/CrowdyEntityTypes.h"
#include "Engine/StreamableManager.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyEntitySubsystem.generated.h"

class FCrowdyServiceRegistry;
class UCrowdyEntityComponent;
class UCrowdyGameSession;
class UCrowdySDKBridgeSubsystem;
enum class ECrowdyOwnership : uint8;
struct FCrowdyDelivery;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyEntityRegistered, const FGuid&, NetID);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyEntityUnregistered, const FGuid&, NetID);
// Fired on the entity's current authority when another client requests ownership of it. RequesterActor is the
// requester's avatar when present on this client (else null); RequesterID is always the requesting player's id.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCrowdyOwnershipRequested, AActor*, TargetEntity, AActor*, RequesterActor, const FGuid&, RequesterID);
// Fired on every client once an entity's ownership actually changes. NewOwnerID is invalid for a host-owned entity.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnCrowdyEntityOwnershipChanged, AActor*, TargetEntity, const FGuid&, NetID, const FGuid&, NewOwnerID, const FGuid&, PreviousOwnerID);

// What a registration attempt did. A Refused result means the registry is unchanged and the caller's participant
// answers to no id at all, so a caller that needs its participant reachable by that id has to react rather than
// carry on as though it holds it.
enum class ECrowdyEntityRegistration : uint8
{
	Registered,
	// The record carried no usable NetID, so there was nothing to register it under.
	RefusedInvalidNetID,
	// A different, still-live participant already holds this NetID and keeps it.
	RefusedIdHeldByLiveParticipant
};

// Largest world coordinate a send may be addressed from. A chunk coordinate is floor(coord / chunk
// size) cast to int64, and that cast is undefined for a value the type cannot hold, so a position past
// this is rejected rather than addressed. It sits far beyond any playable world and far below what the
// smallest sensible chunk size could push out of int64 range.
inline constexpr double CrowdyMaxAddressableWorldCoordinate = 1.0e12;

/**
 * Authoritative FGuid based entity registry for everything SDK replicates.
 * Other systems (actor pool, entity components) register here and delegate
 * their lookups; they no longer keep their own ID maps.
 *
 * Also owns the networked entity lifecycle (spawn/destroy events): it subscribes to the
 * FCrowdyEntitySpawnEvent/FCrowdyEntityDestroyEvent payloads and routes their callbacks to the
 * entity's UCrowdyEntityComponent. Everything between spawn and destroy travels through
 * UCrowdyEventRouter as ordinary game events.
 */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Entity Subsystem"))
class CROWDYREPLICATION_API UCrowdyEntitySubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	//Registry

	void RegisterEntity(const FCrowdyEntityRecord& Record);

	// RegisterEntity, reporting what it did. A registration can be refused - a NetID has only 32 bits of real
	// entropy, so two unrelated participants can derive the same one, and the incumbent keeps it - and the caller
	// that was refused is the party that ends up owning nothing. Refusal is otherwise visible only as a log line
	// on whichever machine hit it, so anything that binds state to an id (a Game Model container, a damage target)
	// should register through this and give up its claim on a Refused result. RegisterEntity is the same call with
	// the answer discarded.
	ECrowdyEntityRegistration TryRegisterEntity(const FCrowdyEntityRecord& Record);

	void UnregisterEntity(const FGuid& NetID);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Subsystem")
	AActor* FindEntity(const FGuid& NetID) const;

	// Widened lookup: returns the participant (an actor or a non-actor UObject), or nullptr. FindEntity stays
	// actor-typed and returns nullptr for a non-actor participant (intended, back-compatible).
	UObject* FindParticipant(const FGuid& NetID) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Subsystem")
	FGuid FindEntityID(const AActor* Actor) const;

	// Widened reverse lookup for any UObject participant. The AActor* overload forwards here; it cannot be a
	// second UFUNCTION of the same name, so this one is plain C++.
	FGuid FindEntityID(const UObject* Participant) const;

	// Enrolls any UObject as a replicated participant with a deterministic, network-stable NetID and returns it.
	// Host participants are world singletons (class-path seed, no salt); LocalClient participants are owner-salted.
	// Returns an invalid guid when the participant is invalid, or when the derived id is already held by a DIFFERENT
	// live participant (logged as an error naming both): an id that answers for someone else is worse than none.
	FGuid RegisterParticipant(UObject* Participant, ECrowdyOwnership Ownership);

	// Enrolls Participant as a sub-participant anchored to an already-registered entity, for a CrowdyContainer
	// component living on a registered actor. The NetID is derived from the anchor's NetID plus the participant's
	// class path and object name, so two same-class components on one actor (and the same component across every
	// client) get distinct, deterministic ids; it inherits the anchor's authority (Role + OwnerID), so a component
	// on a Host-owned boss is Host-owned and one on a player pawn is that player's. Returns the derived NetID, or an
	// invalid guid when the anchor is unregistered, the participant is invalid, or the derived id is already held by
	// a DIFFERENT live participant (a key collision, logged as an error naming both, so an authoring mistake is loud
	// rather than a silent mis-bind). Re-enrolling the same object under the same anchor is idempotent. Unregister
	// via UnregisterParticipant, or let the anchor's teardown drop it. KeyOverride, when non-empty, replaces the
	// object name in the derivation (an author-supplied binding key for a runtime-added component whose object name
	// is not cross-client stable - see ICrowdyBindingKeyProvider); empty keeps the object-name derivation.
	FGuid RegisterSubParticipant(UObject* Participant, const FGuid& AnchorNetID, const FString& KeyOverride = FString());

	// RegisterSubParticipant for an object standing in for a component this machine does not have.
	//
	// An observed entity drawn as a row has no actor, so the CrowdyContainer components its owner enrolled exist
	// here only as a derivation from the class. A stand-in object represents one, and SeedClass and InstanceTerm
	// are the component's own class and the term the owner's enrollment used, so the id derived here is the SAME
	// id the owner minted. That equality is the whole mechanism: the binding reads the owner's row by that key, so
	// a mismatch finds nothing and the row silently keeps its spawn defaults. The record's ClassID is SeedClass's
	// too, which is what lets the binding read the container off the component class rather than off the stand-in.
	FGuid RegisterSubParticipantAs(UObject* Participant, const FGuid& AnchorNetID, const UClass* SeedClass,
		const FString& InstanceTerm);

	// The id RegisterSubParticipantAs would derive, without enrolling anything. Exposed because the derivation is
	// the contract between an owner enrolling a real component and an observer standing in for it, and a caller
	// has to be able to ask whether that id is already held before minting a second object to hold it.
	static FGuid DeriveSubParticipantID(const FGuid& AnchorNetID, const UClass* SeedClass, const FString& InstanceTerm);

	// Removes a participant previously enrolled via RegisterParticipant / RegisterSubParticipant.
	void UnregisterParticipant(UObject* Participant);

	const FCrowdyEntityRecord* FindRecord(const FGuid& NetID) const;

	// Fills in the class of an entity registered before anything named one. Refuses a record that already
	// carries a class: the recorded class decides what an entity declares, so completing a missing fact is
	// safe where repointing an established one is not. False when the id is unknown or already classed.
	bool TrySetRecordClassID(const FGuid& NetID, uint32 ClassID);

	// True when this client is the entity's owner: the record names the local player AND its role says this client
	// simulates and sends for it. Both are required because this answer authorizes creating server-side state bound
	// to the id; a record that names the local player while its role says the entity is only mirrored here
	// contradicts itself, and the safe reading of a contradiction is "not mine".
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

#if WITH_DEV_AUTOMATION_TESTS
	// Injects the game session that Initialize() would normally bind, so a headless test (which never runs
	// Initialize) can exercise the real GetHostID() read-through to UCrowdyGameSession.
	void SetGameSessionForTest(UCrowdyGameSession* InGameSession) { GameSession = InGameSession; }

	// Takes the spawn/destroy subscription against a registry a test owns, using the same call Initialize
	// makes, so a test drives the production delivery path rather than a copy of it.
	void SubscribeToSpawnDestroyForTest(FCrowdyServiceRegistry& Registry) { SubscribeToSpawnDestroy(Registry); }

	// Applies a decoded spawn event through the production handler, so a test can exercise what a peer's event is
	// allowed to do to an existing record without standing up the transport that carried it.
	void HandleRemoteSpawnForTest(const FCrowdyEntitySpawnEvent& Event) { HandleRemoteSpawn(Event); }

	// How many participant-to-id mappings are held. A mapping for a participant that has already been collected
	// cannot be found by key, so this count is the only way to see one that was left behind.
	int32 GetParticipantMappingCountForTest() const { return ParticipantToID.Num(); }
#endif

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
	 *
	 * The spatial transport addresses by region, so the event is sent from Context's current
	 * location. This is the whole of the send: it reads that location and forwards to
	 * DispatchGameEventAt.
	 */
	void DispatchGameEvent(const AActor* Context, FInstancedStruct&& Payload,
		ECrowdyTarget Target = ECrowdyTarget::Everyone, const AActor* TargetEntity = nullptr,
		ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay,
		ECrowdyReplicationDistance ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks);

	/**
	 * DispatchGameEvent for a sender that has no actor to read a position from: the world location the
	 * event is sent from is given directly. Everything else is identical, and the actor overload is
	 * implemented by calling this one, so there is a single send.
	 *
	 * Location decides which region of the world the event reaches, so a caller that passes one in is
	 * asserting where the sender is. Only pass a location the sender itself produced, from its own
	 * position state. A location taken from further up the call chain, or handed in by whoever asked
	 * for the send, lets one sender place its events anywhere in the world.
	 */
	void DispatchGameEventAt(const FVector& Location, FInstancedStruct&& Payload,
		ECrowdyTarget Target = ECrowdyTarget::Everyone, const AActor* TargetEntity = nullptr,
		ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay,
		ECrowdyReplicationDistance ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks);

	/**
	 * The two sends above, taking a BORROWED view of the payload instead of an FInstancedStruct, for a
	 * caller that already holds the value and would otherwise build and copy one only to send it. The
	 * frame is identical; only the ownership differs.
	 *
	 * The view is read during the call and never retained, so the value has to outlive the call and
	 * nothing more. These are synchronous by construction for that reason: a caller that needs the send
	 * deferred must own the payload and use the FInstancedStruct overloads.
	 */
	void DispatchGameEventView(const AActor* Context, const UScriptStruct* PayloadStruct, const void* PayloadMemory,
		ECrowdyTarget Target = ECrowdyTarget::Everyone, const AActor* TargetEntity = nullptr,
		ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay,
		ECrowdyReplicationDistance ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks);

	void DispatchGameEventViewAt(const FVector& Location, const UScriptStruct* PayloadStruct, const void* PayloadMemory,
		ECrowdyTarget Target = ECrowdyTarget::Everyone, const AActor* TargetEntity = nullptr,
		ECrowdyDecayRate DecayRate = ECrowdyDecayRate::No_Decay,
		ECrowdyReplicationDistance ReplicationDistance = ECrowdyReplicationDistance::Eight_Chunks);

private:

	/**
	 * Who the server should deliver a game event to and which chunk it is sent from, shared by both sends
	 * so the owning and borrowing paths cannot drift on routing. False means the event was dropped and the
	 * reason already logged.
	 */
	bool ResolveGameEventRouting(ECrowdyTarget Target, const AActor* TargetEntity, const FVector& Location,
		FGuid& OutTargetID, int64& OutChunkX, int64& OutChunkY, int64& OutChunkZ);

public:

	/**
	 * Sends a payload to the single client that owns TargetActor (a registered entity), using the
	 * actor-to-actor transport. The server delivers it only to that owner no spatial broadcast,
	 * no echo to the sender. The chunk is taken from TargetActor's current location.
	 */
	void DispatchSingleActorMessage(const AActor* TargetActor, FInstancedStruct Payload);

	/**
	 * DispatchSingleActorMessage for a destination with no actor to read from: the registered NetID the
	 * server routes by and the world location its chunk is derived from are given directly. Everything
	 * else is identical, and the actor overload is implemented by calling this one, so there is a single
	 * send and a single chunk derivation.
	 *
	 * Location decides which region of the world the message is addressed to, so only pass a location
	 * read from the destination entity's own live state. A location taken from further up the call chain
	 * lets a caller address a chunk the destination is not standing in.
	 */
	void DispatchSingleActorMessageTo(const FGuid& TargetNetID, const FVector& Location, FInstancedStruct Payload);

	/**
	 * Publishes an already-encoded reliable RPC payload over the named channel (empty = the app-wide
	 * default session channel). It reaches every member of that channel regardless of distance,
	 * bypassing the spatial path's decay and range thinning. The bytes are produced by
	 * FCrowdyRPC::EncodeChannelRpc.
	 */
	void PublishReliableRpc(const FString& ChannelName, const TArray<uint8>& ChannelPayload);

	/** Returns all actors whose entity record matches the given owner UUID. */
	TArray<AActor*> GetEntitiesByOwner(const FGuid& OwnerID) const;

	// Explicit ownership transfer (view plane). Re-points a registered entity's owner and re-derives its role, then
	// broadcasts OnEntityOwnershipChanged so the state replicator re-tracks and the entity component syncs its cache
	// and continuous channel. NewOwnerID: a valid player id makes it client-owned (Owner on that client,
	// RemoteProxy elsewhere); an invalid (zero) id makes it host-owned (a world entity). ExpectedPreviousOwnerID,
	// when valid, is a compare-and-swap guard: the change is dropped when the record's current owner no longer
	// matches, so a stale or duplicate grant is a no-op. This is the client-authoritative view plane - a
	// convention, not an enforcement boundary; cheat-sensitive ownership belongs in a Game Model.
	void ReassignOwnership(const FGuid& NetID, const FGuid& NewOwnerID, const FGuid& ExpectedPreviousOwnerID);

	// Broadcasts OnOwnershipRequested for TargetEntity, resolving the requester's representative actor (its avatar,
	// when present on this client) from RequesterID. Called by the entity's authority when a transfer is requested.
	void NotifyOwnershipRequested(AActor* TargetEntity, const FGuid& RequesterID);

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Subsystem|Events")
	FOnCrowdyEntityRegistered OnEntityRegistered;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Subsystem|Events")
	FOnCrowdyEntityUnregistered OnEntityUnregistered;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Subsystem|Events")
	FOnCrowdyOwnershipRequested OnOwnershipRequested;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Subsystem|Events")
	FOnCrowdyEntityOwnershipChanged OnEntityOwnershipChanged;

private:

	// Remote spawn whose class is still streaming in. A destroy event arriving
	// during the load cancels the spawn. Router events targeted at the entity
	// while it loads are dropped senders put initial data in the spawn payload.
	struct FPendingRemoteSpawn
	{
		FCrowdyEntitySpawnEvent SpawnEvent;
		TSharedPtr<FStreamableHandle> LoadHandle;
	};

	TMap<FGuid, FCrowdyEntityRecord> Records;
	TMap<TObjectKey<UObject>, FGuid> ParticipantToID;
	TMap<FGuid, FPendingRemoteSpawn> PendingRemoteSpawns;

	UPROPERTY()
	TObjectPtr<UCrowdyGameSession> GameSession;

	UPROPERTY()
	FGuid LocalPlayerID;

	UCrowdySDKBridgeSubsystem* Bridge = nullptr;

	// Single handle over both the spawn and destroy payload keys; released on Deinitialize.
	FCrowdySubscription SpawnDestroySubscription;

	// Claims the spawn and destroy payload keys on Registry and points them at HandleSpawnDestroyDelivery.
	void SubscribeToSpawnDestroy(FCrowdyServiceRegistry& Registry);

	// Delivery callback for the spawn/destroy subscription: applies the decoded payload immediately.
	// Routing already narrowed this to the two claimed structs, so no further type check is needed here.
	void HandleSpawnDestroyDelivery(const FCrowdyDelivery& Delivery);

	UFUNCTION()
	void OnOwnerUUIDUpdated(FString NewUUID);

	// Re-derives a LocalClient participant's owner-salted deterministic identity once the local player id arrives,
	// re-registering it under the new NetID. Never touches actor entities (their identity is set at spawn).
	void RestampParticipantIdentity(const FGuid& OldNetID);

	void HandleRemoteSpawn(const FCrowdyEntitySpawnEvent& Event);
	void HandleRemoteDestroy(const FCrowdyEntityDestroyEvent& Event);

	void OnRemoteSpawnClassLoaded(FGuid EntityID, FSoftClassPath ClassPath);
	void FinishRemoteSpawn(const FCrowdyEntitySpawnEvent& Event, UClass* EntityClass);

	// Drops a non-actor stand-in registered for NetID (a crowd avatar enrolled from position updates before the
	// spawn event that names the entity's class arrived), so the actor about to be spawned for that id can take
	// the record. Does nothing unless the record's participant is live, is not an actor, and the record is a
	// remote proxy: a record this client owns or hosts is never displaced by a peer's event.
	void ReleaseRemoteStandIn(const FGuid& NetID);

	// Guarantees lifecycle callbacks have a home when the class ships without a
	// component. Identity is injected before RegisterComponent so the component's
	// BeginPlay registers the record instead of minting its own ID.
	UCrowdyEntityComponent* AddStaticEntityComponent(AActor* Actor, const FGuid& EntityID,
		const FGuid& EntityOwnerID, ECrowdyRole Role, uint32 ClassID) const;

	void DestroyAfterDelay(AActor* Actor, float Delay) const;
};
