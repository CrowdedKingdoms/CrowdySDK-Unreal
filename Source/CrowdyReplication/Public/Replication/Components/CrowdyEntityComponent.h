#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Data/CrowdyEntityTypes.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/Interfaces/CrowdyReplicationSource.h"
#include "Replication/RPC/CrowdyEvent.h"
#include "StructUtils/InstancedStruct.h"
#include "CrowdyEntityComponent.generated.h"

class UCrowdyAutoReplicator;
class UCrowdyEntitySubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCrowdyEntitySpawned, const FInstancedStruct&, InitialState, bool, bIsLocallyOwned);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyEntityDestroyed, bool, bIsLocallyOwned);

UENUM(BlueprintType)
enum class ECrowdyEntityMode : uint8
{
	// Continuous state channel the AutoReplicator sends the StateExecutor's
	// snapshot every replication interval (movement, anything high-frequency).
	Dynamic UMETA(DisplayName="Dynamic"),
	// Event-only state travels through SendEvent and CrowdyEvent handlers.
	Static UMETA(DisplayName="Static"),
};

UENUM(BlueprintType)
enum class ECrowdyIdentityPolicy : uint8
{
	// Deterministic from the logged-in UserID. Only valid on the locally
	// controlled player pawn; anything else falls back to Random with a warning.
	PlayerDerived UMETA(DisplayName="Player Derived"),
	// Hashed from the owner's level path every client computes the same NetID
	// for the same level-placed actor without hand-typed seeds.
	Stable UMETA(DisplayName="Stable"),
	Random UMETA(DisplayName="Random"),
};

UENUM(BlueprintType)
enum class ECrowdyOwnership : uint8
{
	// This client owns and simulates the entity (Role=Owner). The authority axis; orthogonal to Mode and
	// IdentityPolicy. A runtime host-spawned entity via SpawnEntity is already Owner-on-host (de-facto
	// host-authoritative), so this axis is for level-placed world entities.
	LocalClient UMETA(DisplayName="Local Client"),
	// Owned by whichever client is currently host (Role=HostOwned, no per-client owner). For level-placed
	// world/AI entities that must share one authority across every client.
	Host UMETA(DisplayName="Host"),
};

UENUM(BlueprintType)
enum class ECrowdyHostOverride : uint8
{
	// The elected host may override this client-owned entity's CrowdyState as a super-user; the owner adopts it.
	Allow UMETA(DisplayName="Allow"),
	// Only the owning client may change this entity; even a HostSourced correction is dropped.
	OwnerOnly UMETA(DisplayName="Owner Only"),
};

UENUM(BlueprintType)
enum class ECrowdyStateHeartbeat : uint8
{
	// Follow the map profile: this entity's CrowdyHeartbeat-marked properties are re-sent every
	// StateKeyframeIntervalSeconds (when the map's keyframe interval is > 0).
	Inherit UMETA(DisplayName="Inherit"),
	// Never emit a keyframe heartbeat for this entity, regardless of its property marks or the map setting; a
	// kill switch for always-active entities. On-change replication is unaffected either way.
	Off UMETA(DisplayName="Off"),
};

/**
 * The single replication component for everything the SDK tracks as an entity.
 * Registers the owner in UCrowdyEntitySubsystem, optionally feeds the continuous
 * state channel (Dynamic mode), and receives the entity lifecycle callbacks.
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent, DisplayName="Crowdy Entity Component"))
class CROWDYREPLICATION_API UCrowdyEntityComponent : public UActorComponent, public ICrowdyReplicationSource
{
	GENERATED_BODY()

	// Configures Mode when adding the component dynamically to remote spawns
	friend class UCrowdyEntitySubsystem;

public:

	UCrowdyEntityComponent();

	/**
	 * Injects identity before BeginPlay so the component skips ID minting.
	 * Used by UCrowdyEntitySubsystem for deferred spawn, the original class is
	 * spawned on every client, with role/ownership decided by the spawn event.
	 */
	void InitIdentity(const FGuid& InNetID, const FGuid& InOwnerID, ECrowdyRole InRole, uint32 InClassID);

	/**
	 * Assigns identity to an actor that has already begun play for pooled proxies.
	 * The actor pool pre-warms and reuses actors, so InitIdentity's pre-BeginPlay
	 * injection can't be used. Only syncs the component's own fields; the rendering
	 * backend owns the EntitySubsystem record for pooled proxies. Pair with
	 * ClearIdentity() when the actor returns to the pool.
	 */
	void AssignPooledIdentity(const FGuid& InNetID, const FGuid& InOwnerID, ECrowdyRole InRole, uint32 InClassID);

	/**
	 * Clears identity and unregisters this component's record, returning a pooled actor
	 * to an inert, unassigned state. Also removes the throwaway Owner record a pre-warmed
	 * pool actor mints in BeginPlay before it is assigned a real proxy id.
	 */
	void ClearIdentity();

	// Continuous channel (Dynamic mode)

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void StartReplication();

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void StopReplication();

	// Event channel

	/**
	 * Sends a game event addressed at this entity. Everyone: the entity's
	 * handlers run on every client; OwnerOnly: only on the owning client.
	 * State changes are plain events now handle them with meta=(CrowdyEvent)
	 * functions on the owner.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void SendEvent(const FInstancedStruct& Payload, ECrowdyEventScope Scope = ECrowdyEventScope::Everyone);

	// CrowdyState manual-dirty push. A property marked meta=(CrowdyState, CrowdyManualDirty) is never
	// auto-diffed; call these to schedule it (or all of this entity's manual-dirty properties) to ship on the
	// next replication tick. No-op if this entity is not driven by the state replicator (e.g. a proxy, or
	// bUseStateReplicator is off).

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void MarkStateDirty(FName PropertyName);

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void MarkAllStateDirty();

	/** Broadcasts an FCrowdyEntityDestroyEvent, then destroys the owner (after DestroyDelay). */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void DestroyEntity();

	// Explicit ownership transfer (request/grant on the client-authoritative view plane). Prefer the static
	// UCrowdyOwnershipTransfer nodes over calling these directly.

	// RPC receiver (Multicast over the reliable session channel): runs on every client. On the entity's current
	// authority (its owning client, or the host for a host-owned world entity) it either auto-grants when
	// bAutoApproveOwnershipRequests is set, or broadcasts UCrowdyEntitySubsystem::OnOwnershipRequested for game
	// code to decide; on every other client it is a no-op. RequesterID is the player asking to own the entity.
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "Multicast"))
	void RequestOwnership_Implementation(FGuid RequesterID);
	CROWDY_EVENT(RequestOwnership)

	// RPC receiver (Multicast over the reliable session channel): runs on every client and re-points this
	// entity's owner via UCrowdyEntitySubsystem::ReassignOwnership, which compare-and-swaps on PreviousOwnerID so
	// a stale or duplicate grant is dropped. NewOwnerID is the player to own it, or an invalid guid to make it
	// host-owned (a world entity).
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "Multicast"))
	void GrantOwnership_Implementation(FGuid NewOwnerID, FGuid PreviousOwnerID);
	CROWDY_EVENT(GrantOwnership)

	// Grants this entity to NewOwnerID (an invalid guid makes it host-owned), announcing the change to every
	// client. No-op unless this client is the entity's current authority. Because GrantOwnership is a Multicast,
	// its body also runs locally here, so the granting authority relinquishes/adopts without a second call.
	// Called by the static grant nodes and by the auto-approve path.
	void GrantOwnershipTo(const FGuid& NewOwnerID);

	// Lifecycle events

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Component")
	FOnCrowdyEntitySpawned OnCrowdySpawned;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Component")
	FOnCrowdyEntityDestroyed OnCrowdyDestroyed;

	/** Seconds between OnCrowdyDestroyed firing and the actor being destroyed (dissolve effects etc.); 0 = immediate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Entity Component")
	float DestroyDelay = 0.f;

	// Accessors
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	FGuid GetNetID() const { return NetID; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	FGuid GetOwnerID() const { return OwnerID; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	ECrowdyRole GetRole() const { return Role; }

	// True when this client is the authority for the entity: for LocalClient ownership, when we are its owner
	// (Role==Owner); for Host ownership, when we are the elected host (Role==HostOwned and local id == host id).
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	bool IsLocallyOwned() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	ECrowdyEntityMode GetMode() const { return Mode; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	ECrowdyOwnership GetOwnership() const { return Ownership; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	ECrowdyHostOverride GetHostOverridePolicy() const { return HostOverride; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	ECrowdyStateHeartbeat GetStateHeartbeat() const { return StateHeartbeat; }

	// Pure authority derivation from the Ownership axis, shared by ResolveIdentity and the unit tests. Host ->
	// HostOwned with no owner id (world entity); LocalClient -> Owner with the local player id.
	static void DeriveAuthority(ECrowdyOwnership InOwnership, const FGuid& InLocalPlayerID, ECrowdyRole& OutRole, FGuid& OutOwnerID);

	// Pure ownership test behind UCrowdyUtilities::DoesCrowdyEntityOwn, shared with the unit tests. A HostOwned
	// entity carries no per-player owner id (OwnerID is Guid::Zero by DeriveAuthority), so it is resolved to the
	// concrete host id on BOTH sides: as the owner it acts as the host; as the target it is owned by the host.
	// Player entities are unchanged — an owner acts under its own NetID (a player avatar's NetID is its player
	// id), a target is owned by its stamped OwnerID. Fails closed: if a HostOwned side has no host id, no match.
	static bool DoesOwnershipMatch(ECrowdyRole OwnerRole, const FGuid& OwnerNetID,
		ECrowdyRole TargetRole, const FGuid& TargetOwnerID, const FGuid& HostID);

	uint32 GetClassID() const { return ClassID; }

	// ICrowdyReplicationSource
	virtual AActor* GetReplicatedActor() const override { return CachedOwner; }
	virtual const FString& GetReplicationUUID() const override { return UUIDString; }
	virtual const FInstancedStruct& GetReplicatedState() const override;

	// Component config settable from an owning actor's constructor;
	// EditAnywhere keeps them tweakable on instances and Blueprint defaults.
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component",
		meta=(ToolTip="Continuous StateExecutor/AutoReplicator channel (Dynamic) vs event-only (Static). Unrelated to ownership or CrowdyState property replication."))
	ECrowdyEntityMode Mode = ECrowdyEntityMode::Dynamic;

	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component")
	ECrowdyIdentityPolicy IdentityPolicy = ECrowdyIdentityPolicy::Stable;

	// Authority axis: who owns and simulates this entity. Orthogonal to Mode and IdentityPolicy. LocalClient
	// (default): this client owns it (Role=Owner). Host: whichever client is host owns it (Role=HostOwned) for
	// level-placed world entities. Consumed only on the self-resolving (level-placed / Stable) identity path;
	// injected-identity spawns pass an explicit Role and ignore this.
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component")
	ECrowdyOwnership Ownership = ECrowdyOwnership::LocalClient;

	// For a LocalClient-owned entity only: whether the elected host may override its CrowdyState as a super-user.
	// Allow (default): host corrections are accepted and adopted. OwnerOnly: only the owner changes it. This is a
	// coordination convention on the unenforced view plane, NOT a security boundary cheat-sensitive state belongs
	// in Game Models. Hidden when Ownership==Host (a host-owned entity has no separate owner to protect).
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component",
		meta=(EditCondition="Ownership == ECrowdyOwnership::LocalClient", EditConditionHides))
	ECrowdyHostOverride HostOverride = ECrowdyHostOverride::Allow;

	// Per-entity CrowdyState keyframe-heartbeat control. Inherit (default): this entity's CrowdyHeartbeat-marked
	// properties ride the periodic heartbeat when the map enables it. Off: this entity never emits a keyframe
	// heartbeat, a kill switch for always-active entities. On-change replication is unaffected either way; the
	// per-property CrowdyHeartbeat opt-in still decides which properties a heartbeat would carry.
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component")
	ECrowdyStateHeartbeat StateHeartbeat = ECrowdyStateHeartbeat::Inherit;

	// When true, this entity's current authority (its owning client, or the host for a host-owned entity) grants
	// any ownership-transfer request immediately, instead of surfacing UCrowdyEntitySubsystem::OnOwnershipRequested
	// for game code to decide. Authored config, read on whichever client is the authority when a request arrives.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Entity Component")
	bool bAutoApproveOwnershipRequests = false;

	// Dynamic mode only produces the snapshot the AutoReplicator sends
	UPROPERTY(EditAnywhere, Instanced, Category="Crowdy SDK|Entity Component",
		meta=(EditCondition="Mode == ECrowdyEntityMode::Dynamic", EditConditionHides))
	TObjectPtr<UActorUpdateExecutor> StateExecutor;

	// Dynamic mode: join the AutoReplicator on BeginPlay (StartReplication otherwise)
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component",
		meta=(EditCondition="Mode == ECrowdyEntityMode::Dynamic", EditConditionHides))
	bool bAutoRegister = true;

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	UPROPERTY()
	FGuid NetID;

	UPROPERTY()
	FGuid OwnerID;

	UPROPERTY()
	ECrowdyRole Role = ECrowdyRole::None;

	// Hash of the owner's class path, carried on spawn events
	UPROPERTY()
	uint32 ClassID = 0;

	// Derived from NetID once; exists only for the actor-update wire format
	UPROPERTY()
	FString UUIDString;

	UPROPERTY()
	mutable FInstancedStruct CachedState;

	UPROPERTY()
	TObjectPtr<AActor> CachedOwner;

	UPROPERTY()
	TObjectPtr<UCrowdyAutoReplicator> AutoReplicator;

	UPROPERTY()
	TObjectPtr<UCrowdyEntitySubsystem> EntitySubsystem;

	bool bIdentityInjected = false;

	void ResolveIdentity();

	// Applied by UCrowdyEntitySubsystem::ReassignOwnership (a friend) after the entity record is re-pointed: syncs
	// this component's cached OwnerID/Role and, for a Dynamic-mode entity, moves the continuous StateExecutor
	// channel to follow authority (the new owner joins the AutoReplicator; a client that lost ownership leaves it).
	void ApplyOwnershipReassignment(const FGuid& NewOwnerID, ECrowdyRole NewRole);
};
