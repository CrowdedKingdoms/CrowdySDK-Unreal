#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Data/CrowdyEntityTypes.h"
#include "Engine/TimerHandle.h"
#include "Replication/Executor/ActorUpdateExecutor.h"
#include "Replication/Interfaces/CrowdyReplicationSource.h"
#include "Replication/RPC/CrowdyEvent.h"
#include "StructUtils/InstancedStruct.h"
#include "CrowdyEntityComponent.generated.h"

class UCrowdyAutoReplicator;
class UCrowdyEntitySubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCrowdyEntitySpawned, const FInstancedStruct&, InitialState, bool, bIsLocallyOwned);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyEntityDestroyed, bool, bIsLocallyOwned);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCrowdyOwnershipAssigned, const FGuid&, NewOwnerID, ECrowdyRole, NewRole, bool, bIsLocallyOwned);

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

// Why PlayerDerived identity did or did not resolve from the signed-in account. PlayerDerived hashes the local
// player's account id, so exactly one actor per client can use it: the locally controlled primary player pawn.
// Every other answer falls back to a random id, which no other client can compute or address.
enum class ECrowdyPlayerDerivedIdentity : uint8
{
	Resolved,
	NotAPawn,
	PawnHasNoController,
	ControllerIsNotAPlayer,
	ControllerIsRemote,
	NotThePrimaryPlayer,
	NoGameSession,
};

// The facts PlayerDerived identity is decided from, read off the engine at the point of decision so the rule
// itself stays pure and testable. Each field is one question with one answer; the classifier reports the first
// one that fails, so the message names the condition actually measured rather than a summary of all of them.
struct FCrowdyPlayerDerivedIdentityFacts
{
	bool bIsPawn = false;
	bool bHasController = false;
	bool bControllerIsPlayerController = false;
	bool bIsLocalController = false;
	bool bIsPrimaryPlayer = false;
	bool bHasGameSession = false;
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

	// Fires on every client once this entity knows who owns it, and again every time ownership moves. The first
	// announcement lands on the tick after the entity is registered, so a Blueprint that binds in its owner's
	// BeginPlay still receives it. NewOwnerID is invalid for a host-owned world entity; bIsLocallyOwned is the
	// answer this client should act on (see IsLocallyOwned).
	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Component")
	FOnCrowdyOwnershipAssigned OnCrowdyOwnershipAssigned;

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

	// The ownership actually applied when an entity resolves its own identity. A level-placed world actor follows
	// the ownership it was authored with; an actor spawned at runtime is owned by the client that spawned it, no
	// matter what the asset says, because there is no other client that could claim it. Pure, so it is shared by
	// ResolveIdentity and the unit tests.
	static ECrowdyOwnership ResolveEffectiveOwnership(ECrowdyOwnership InAuthoredOwnership, bool bIsLevelPlaced);

	// The engine's per-placement identity for Actor, or an invalid guid when it has none. This is the Stable
	// identity seed. It is not a test for "was this actor placed in the level": the guid is editor-only data, and
	// in the editor a runtime-spawned actor is given one too. AActor::IsNetStartupActor answers that question.
	static FGuid ResolveActorInstanceGuid(const AActor* Actor);

	// Pure ownership test behind UCrowdyUtilities::DoesCrowdyEntityOwn, shared with the unit tests. A HostOwned
	// entity carries no per-player owner id (OwnerID is Guid::Zero by DeriveAuthority), so it is resolved to the
	// concrete host id on BOTH sides: as the owner it acts as the host; as the target it is owned by the host.
	// Player entities are unchanged: an owner acts under its own NetID (a player avatar's NetID is its player
	// id), a target is owned by its stamped OwnerID. Fails closed: if a HostOwned side has no host id, no match.
	static bool DoesOwnershipMatch(ECrowdyRole OwnerRole, const FGuid& OwnerNetID,
		ECrowdyRole TargetRole, const FGuid& TargetOwnerID, const FGuid& HostID);

	// Which PlayerDerived condition decided this entity's identity, from the facts read at the decision point.
	// Reports the FIRST unmet condition, so a bare actor is answered "not a Pawn" and never "no game session":
	// the later questions are not asked of an actor the earlier ones already rule out.
	static ECrowdyPlayerDerivedIdentity ClassifyPlayerDerivedIdentity(const FCrowdyPlayerDerivedIdentityFacts& Facts);

	// The clause naming that verdict in a diagnostic, phrased as the condition that was actually measured.
	static const TCHAR* DescribePlayerDerivedIdentity(ECrowdyPlayerDerivedIdentity Verdict);

	// Whether a Dynamic entity with Auto Register ticked ends BeginPlay without joining the continuous state
	// channel because it is host-owned. Auto Register starts the channel only for an entity this client owns
	// outright, so a host-owned entity leaves BeginPlay sending nothing at all.
	static bool ShouldReportHostOwnedAutoRegister(ECrowdyEntityMode InMode, bool bInAutoRegister, ECrowdyRole InRole);

	// How this client currently answers "who is the host", for a diagnostic that must not overstate what it knows.
	// A client with no host id has not been told yet, which reads the same whether election is still running or
	// there is no entity subsystem to ask.
	static const TCHAR* DescribeHostElectionState(bool bHostKnown, bool bLocalClientIsHost);

	uint32 GetClassID() const { return ClassID; }

	// Whether the engine's placement guid may be used as this entity's shared id. It may only when the actor was
	// loaded with its level AND the engine actually has a guid for it. The level-placed half is what makes the id
	// shared: the guid is saved in the level, so every client reads the same one. An actor created during play has
	// no such shared origin, and asking the engine for its guid answers with a fresh per-run, per-client value in
	// the editor and with nothing at all in a packaged build, so honouring it would make the two disagree.
	static bool ShouldUsePlacementGuid(bool bIsLevelPlaced, bool bHasInstanceGuid);

	// The deterministic Stable-identity NetID for this actor. For an actor placed in the level this is the engine's
	// own per-placement identity (unique per placement of an instanced/streamed level, identical on every client,
	// saved into the level, and surviving World Partition embedding), which is exactly the distinctness the SDK
	// needs. Anything else falls back to hashing the actor path, and bOutUsedPathFallback reports which was used.
	// Note that the fallback cannot produce an id other clients agree on for an actor created during play; there is
	// nothing shared to derive one from. Such an actor should be spawned through UCrowdyEntitySubsystem::SpawnEntity,
	// which mints an id and announces it, or given a binding key.
	FGuid ComputeStableNetID(bool& bOutUsedPathFallback) const;

#if WITH_DEV_AUTOMATION_TESTS
	// Drives the private identity resolution for a headless test: injects the owner, runs ResolveIdentity, returns
	// the resolved NetID. EntitySubsystem stays null (no registration side effects), so only the derivation runs -
	// the keyed path and the empty-key fall-through are exercised without a world or a session.
	FGuid ResolveIdentityForTest(AActor* Owner)
	{
		CachedOwner = Owner;
		EntitySubsystem = nullptr;
		ResolveIdentity();
		return NetID;
	}

	// Runs the deferred first ownership announcement now, so a headless test can drive the one-shot and
	// no-identity gates without ticking a world.
	void AnnounceInitialOwnershipForTest() { AnnounceInitialOwnership(); }

	// Drives the two authoring diagnostics directly, so a headless test can prove each speaks once per component
	// instance without standing up the world their real call sites need.
	void ReportPlayerDerivedFallbackForTest(const ECrowdyPlayerDerivedIdentity Verdict) { ReportPlayerDerivedFallback(Verdict); }
	void ReportHostOwnedAutoRegisterSkippedForTest() { ReportHostOwnedAutoRegisterSkipped(); }

	// How many times each diagnostic has spoken on this instance, and the last thing each said.
	int32 PlayerDerivedFallbackReportCount = 0;
	int32 HostOwnedAutoRegisterReportCount = 0;
	FString LastPlayerDerivedFallbackMessage;
	FString LastHostOwnedAutoRegisterMessage;

	// How many times OnCrowdyOwnershipAssigned has been announced, and the values carried by the last one.
	int32 OwnershipAnnouncementCount = 0;
	FGuid LastAnnouncedOwnerID;
	ECrowdyRole LastAnnouncedRole = ECrowdyRole::None;
	bool bLastAnnouncedLocallyOwned = false;

	// Ordering instrumentation for the headless tests, compiled out of a shipping build. Each step of an ownership
	// reassignment stamps the shared counter, so a test can assert that the entity announces to its own listeners
	// only after every system tracking the entity has been re-pointed at the new owner.
	static int32 OwnershipStepCounter;
	int32 TrackersRepointedStep = 0;
	int32 LastAnnouncementStep = 0;
#endif

	// ICrowdyReplicationSource
	virtual AActor* GetReplicatedActor() const override { return CachedOwner; }
	virtual const FString& GetReplicationUUID() const override { return UUIDString; }
	virtual const FInstancedStruct& GetReplicatedState() const override;

	// Component config settable from an owning actor's constructor;
	// EditAnywhere keeps them tweakable on instances and Blueprint defaults.

	// Which channel this entity feeds. Static (default): event-only, nothing is sent every tick. Dynamic: the
	// continuous StateExecutor/AutoReplicator snapshot channel, for movement and anything else high-frequency;
	// it also reveals the StateExecutor and Auto Register settings, which only that channel uses. Unrelated to
	// ownership and to CrowdyState property replication, both of which work in either mode.
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component",
		meta=(ToolTip="Event-only (Static) vs the continuous StateExecutor/AutoReplicator channel (Dynamic). Unrelated to ownership or CrowdyState property replication."))
	ECrowdyEntityMode Mode = ECrowdyEntityMode::Static;

	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component")
	ECrowdyIdentityPolicy IdentityPolicy = ECrowdyIdentityPolicy::Stable;

	// Authority axis: who owns and simulates this entity. Orthogonal to Mode and IdentityPolicy. Host (default):
	// whichever client is currently host owns it (Role=HostOwned), which is what a level-placed world or AI entity
	// wants so every client shares one authority. LocalClient: this client owns it (Role=Owner). This applies to
	// level-placed actors; an actor spawned at runtime is always owned by the client that spawned it, whatever is
	// set here. Consumed only on the self-resolving identity path; injected-identity spawns pass an explicit Role
	// and ignore this.
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component")
	ECrowdyOwnership Ownership = ECrowdyOwnership::Host;

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

	// The executor class the answer below was resolved for. A Blueprint recompile reinstances the executor
	// into a class that may now override the event, and this component's BeginPlay does not re-run, so the
	// answer is keyed on the class rather than taken once.
	mutable TWeakObjectPtr<const UClass> ResolvedStateExecutorClass;

	mutable bool bStateExecutorAnswersNatively = false;

	UPROPERTY()
	TObjectPtr<AActor> CachedOwner;

	UPROPERTY()
	TObjectPtr<UCrowdyAutoReplicator> AutoReplicator;

	UPROPERTY()
	TObjectPtr<UCrowdyEntitySubsystem> EntitySubsystem;

	bool bIdentityInjected = false;

	// True once OnCrowdyOwnershipAssigned has announced this identity, so the first announcement happens exactly
	// once. Cleared by ClearIdentity so a pooled actor announces again when it is handed a new identity.
	bool bOwnershipAnnounced = false;

	// Holds the pending first announcement so it can be cancelled if the entity goes away first.
	FTimerHandle InitialOwnershipTimer;

	// One report per component instance for each authoring diagnostic below. Both describe how this component was
	// configured, which does not change while it lives, so a second report would only repeat the first. They are
	// deliberately not cleared by ClearIdentity: a pooled actor keeps the configuration it was already told about.
	bool bPlayerDerivedFallbackReported = false;
	bool bHostOwnedAutoRegisterReported = false;

	void ResolveIdentity();

	// Says that PlayerDerived identity fell back to a random id, which condition caused it, and what to set
	// instead. Speaks once per component instance.
	void ReportPlayerDerivedFallback(ECrowdyPlayerDerivedIdentity Verdict);

	// Says that a host-owned entity finished BeginPlay without joining the continuous state channel despite Auto
	// Register being ticked, what this client currently knows about host election, and what to set or call
	// instead. Speaks once per component instance.
	void ReportHostOwnedAutoRegisterSkipped();

	// Announces the entity's current owner and role on OnCrowdyOwnershipAssigned. A listener is free to destroy
	// the owning actor, so callers must treat this as the last thing they do with member state. On a reassignment
	// UCrowdyEntitySubsystem (a friend) calls this only after every system tracking the entity has been re-pointed,
	// so a listener that immediately writes and marks replicated state is not doing so on an untracked entity.
	void BroadcastOwnershipAssigned();

	// Queues the first announcement for the next tick. Deferring it means the entity is announced after its own
	// actor has begun play (a component begins play first), so Blueprints that bind in BeginPlay still hear it,
	// and after a pre-warmed pooled actor has had its throwaway identity taken away again.
	void ScheduleInitialOwnershipAnnouncement();

	// Deferred body of ScheduleInitialOwnershipAnnouncement. Announces only once, and only for an entity that
	// still holds an identity, so a pooled actor sitting idle in the pool announces nothing.
	void AnnounceInitialOwnership();

	// Applied by UCrowdyEntitySubsystem::ReassignOwnership (a friend) after the entity record is re-pointed: syncs
	// this component's cached OwnerID/Role and, for a Dynamic-mode entity, moves the continuous StateExecutor
	// channel to follow authority (the new owner joins the AutoReplicator; a client that lost ownership leaves it).
	// It does not announce; the subsystem does that afterwards, via BroadcastOwnershipAssigned.
	void ApplyOwnershipReassignment(const FGuid& NewOwnerID, ECrowdyRole NewRole);
};
