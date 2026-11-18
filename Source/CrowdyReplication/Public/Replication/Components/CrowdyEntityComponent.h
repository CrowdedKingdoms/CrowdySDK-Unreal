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

class AController;
class APawn;
class UCrowdyAutoReplicator;
class UCrowdyEntitySubsystem;
class UCrowdyGameSession;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCrowdyEntitySpawned, const FInstancedStruct&, InitialState, bool, bIsLocallyOwned);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCrowdyEntityDestroyed, bool, bIsLocallyOwned);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCrowdyOwnershipAssigned, const FGuid&, NewOwnerID, ECrowdyRole, NewRole, bool, bIsLocallyOwned);

UENUM(BlueprintType)
enum class ECrowdyIdentityPolicy : uint8
{
	// The session's random actor id, new at each sign-in (a live pawn re-keys to it) and kept across respawns. Only valid on the locally
	// controlled player pawn; anything else falls back to Random with a warning.
	// A pawn spawned during play registers inside its first possession, after the pawn's Possessed and
	// Controller Changed events, and supersedes any earlier pawn still holding the session's id: read the id
	// from OnCrowdyOwnershipAssigned or the entity subsystem's OnEntityRegistered, not from BeginPlay or Possessed.
	PlayerDerived UMETA(DisplayName="Player Derived"),
	// Hashed from the owner's level path every client computes the same NetID
	// for the same level-placed actor without hand-typed seeds.
	Stable UMETA(DisplayName="Stable"),
	Random UMETA(DisplayName="Random"),
};

UENUM(BlueprintType)
enum class ECrowdyOwnership : uint8
{
	// This client owns and simulates the entity (Role=Owner).
	LocalClient UMETA(DisplayName="Local Client"),
	// No client owns it (Role=WorldOwned): a World entity, or a replicated subsystem, which the Leader writes.
	Host UMETA(DisplayName="Host"),
};

// Which client owns an entity: the client that writes its view state.
UENUM(BlueprintType)
enum class ECrowdyOwnedBy : uint8
{
	Auto UMETA(DisplayName="Auto", ToolTip="World when placed in the level, Player when spawned during play."),
	Player UMETA(DisplayName="Player", ToolTip="The player whose client placed or spawned it."),
	World UMETA(DisplayName="World", ToolTip="Placed in the level; no client writes it, so its truth belongs in Server Objects. An entity spawned during play belongs to its spawner instead."),
	Leader UMETA(DisplayName="Leader", ToolTip="Whichever client currently leads. For players who share a map, and NPCs that need navmesh or physics; only the Leader spawns or destroys one."),
};

// Whether an entity sends its movement every replication interval.
UENUM(BlueprintType)
enum class ECrowdyReplicateMovement : uint8
{
	Auto UMETA(DisplayName="Auto", ToolTip="On for a pawn with a movement component, otherwise Off."),
	On UMETA(DisplayName="On", ToolTip="Sends movement every replication interval."),
	Off UMETA(DisplayName="Off", ToolTip="Sends no movement. Events and CrowdyState still work."),
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

// Why PlayerDerived identity did or did not resolve from the signed-in session. PlayerDerived takes the session's
// actor id, so exactly one actor per client can use it: the locally controlled primary player pawn.
// Every other answer falls back to a random id that is not the session's.
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
 * state channel (when it replicates movement), and receives the entity lifecycle callbacks.
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent, DisplayName="Crowdy Entity Component"))
class CROWDYREPLICATION_API UCrowdyEntityComponent : public UActorComponent, public ICrowdyReplicationSource
{
	GENERATED_BODY()

	// Configures Replicate Movement when adding the component dynamically to remote spawns
	friend class UCrowdyEntitySubsystem;

public:

	UCrowdyEntityComponent();

	virtual void Serialize(FArchive& Ar) override;

	/**
	 * Injects identity before BeginPlay so the component skips ID minting.
	 * Used by UCrowdyEntitySubsystem for deferred spawn, the original class is
	 * spawned on every client, with role/ownership decided by the spawn event.
	 */
	void InitIdentity(const FGuid& InNetID, const FGuid& InWriterID, int64 InOwnerUserID, ECrowdyRole InRole, uint32 InClassID);

	/**
	 * Assigns identity to an actor that has already begun play for pooled proxies.
	 * The actor pool pre-warms and reuses actors, so InitIdentity's pre-BeginPlay
	 * injection can't be used. Only syncs the component's own fields; the rendering
	 * backend owns the EntitySubsystem record for pooled proxies. Pair with
	 * ClearIdentity() when the actor returns to the pool.
	 */
	void AssignPooledIdentity(const FGuid& InNetID, const FGuid& InWriterID, int64 InOwnerUserID, ECrowdyRole InRole, uint32 InClassID);

	/**
	 * Clears identity and unregisters this component's record, returning a pooled actor
	 * to an inert, unassigned state.
	 */
	void ClearIdentity();

	/**
	 * Marks an actor the pool is pre-warming, and must be called BEFORE BeginPlay. A dormant component
	 * resolves no identity and registers no entity at all, so nothing ever observes it.
	 *
	 * Unregistering afterwards is not equivalent: registration is delivered synchronously, so every listener
	 * has already acted on an actor that stands for nothing, and no later cleanup recalls that.
	 */
	void MarkPooledDormant();

	// True while this component belongs to an actor the pool is holding pre-warmed. Public so a test can assert
	// the pool marked its actors before finishing their spawn, which is the ordering the defect turned on.
	bool IsPooledDormant() const { return bPooledDormant; }

	// Continuous channel, for an entity that replicates movement

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void StartReplication();

	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Entity Component")
	void StopReplication();

	// Tells the state replicator whether this entity's updates are going out, so continuous properties have a carrier.
	void NoteContinuousCarrier() const;

	/**
	 * Marks an entity spawned with Spawn Entity. One that does not replicate movement then sends its spawn again every few
	 * seconds while this client owns it, so a player who joins later gets it too. One that does is found by its updates.
	 */
	void EnablePresence();

	// True for a body handed its identity by a spawn or the pool, false for one placed in the level.
	bool HasInjectedIdentity() const { return bIdentityInjected; }

	/** Broadcasts OnCrowdySpawned, once per identity, whichever of the spawn event and the first update came first. */
	void DeliverSpawnData(const FInstancedStruct& InitialState, bool bIsLocal);

#if WITH_DEV_AUTOMATION_TESTS
	// What BeginPlay resolves, for an editor world where it never runs; the auto replicator exists only in game and PIE worlds.
	void SetAutoReplicatorForTest(UCrowdyAutoReplicator* InAutoReplicator)
	{
		AutoReplicator = InAutoReplicator;
		CachedOwner = GetOwner();
	}
#endif

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
	// authority (its owning client; a World entity has none) it either auto-grants when
	// bAutoApproveOwnershipRequests is set, or broadcasts UCrowdyEntitySubsystem::OnOwnershipRequested for game
	// code to decide; on every other client it is a no-op. RequesterID is the player asking to own the entity, and
	// RequesterUserID the user that player's session claims (0 when unknown).
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "Multicast"))
	void RequestOwnership_Implementation(FGuid RequesterID, int64 RequesterUserID);
	CROWDY_EVENT(RequestOwnership)

	// RPC receiver (Multicast over the reliable session channel): runs on every client and re-points this
	// entity's owner via UCrowdyEntitySubsystem::ReassignOwnership, which compare-and-swaps on PreviousWriterID so
	// a stale or duplicate grant is dropped. NewWriterID is the player to own it, or an invalid guid to make it
	// a World entity, which no client writes. NewOwnerUserID is that player's user as the granter knows it, or 0.
	UFUNCTION(meta = (CrowdyEvent, CrowdyRecipient = "Multicast"))
	void GrantOwnership_Implementation(FGuid NewWriterID, FGuid PreviousWriterID, int64 NewOwnerUserID);
	CROWDY_EVENT(GrantOwnership)

	// Grants this entity to NewWriterID (an invalid guid makes it a World entity), announcing the change to every
	// client. No-op unless this client is the entity's current authority. Because GrantOwnership is a Multicast,
	// its body also runs locally here, so the granting authority relinquishes/adopts without a second call.
	// Called by the static grant nodes and by the auto-approve path. NewOwnerUserID is 0 when the granter does not know it.
	void GrantOwnershipTo(const FGuid& NewWriterID, int64 NewOwnerUserID);

	// Lifecycle events

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Component")
	FOnCrowdyEntitySpawned OnCrowdySpawned;

	UPROPERTY(BlueprintAssignable, Category="Crowdy SDK|Entity Component")
	FOnCrowdyEntityDestroyed OnCrowdyDestroyed;

	// Fires on every client once this entity knows who owns it, and again every time ownership moves. The first
	// announcement lands on the tick after the entity is registered, so a Blueprint that binds in its owner's
	// BeginPlay still receives it. NewWriterID is invalid for a host-owned world entity; bIsLocallyOwned is the
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
	FGuid GetWriterID() const { return WriterID; }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component",
		meta=(ToolTip="The user id of the player who owns this entity, 0 when unknown. A label for display, not a check that this client writes it."))
	int64 GetOwnerUserID() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component",
		meta=(ToolTip="True when the signed-in user owns this entity, from this session or another. Use Is Locally Owned to decide whether to write it."))
	bool IsOwnedByLocalUser() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	ECrowdyRole GetRole() const { return Role; }

	// True when this client writes the entity: it is its owner (Role==Owner). A World entity has no writer.
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	bool IsLocallyOwned() const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	bool ReplicatesMovement() const;

	// The same for a component template of ActorClass, which has no owner to ask. It walks the class's templates, so
	// resolve it once per class rather than per entity.
	bool ReplicatesMovementFor(const UClass* ActorClass) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy SDK|Entity Component")
	ECrowdyStateHeartbeat GetStateHeartbeat() const { return StateHeartbeat; }

	// The role and owner id for a participant registered with an ownership: Host -> WorldOwned with no owner id
	// (world entity); LocalClient -> Owner with the local player id.
	static void DeriveAuthority(ECrowdyOwnership InOwnership, const FGuid& InLocalPlayerID, ECrowdyRole& OutRole, FGuid& OutWriterID);

	// The ownership that applies to a participant: a level-placed world actor follows the ownership it was authored
	// with; an actor spawned at runtime is owned by the client that spawned it, because no other client could claim it.
	static ECrowdyOwnership ResolveEffectiveOwnership(ECrowdyOwnership InAuthoredOwnership, bool bIsLevelPlaced);

	// Who owns an entity with these settings: Player, World or Leader, never Auto. Only an entity placed in the level
	// is there on every client from the start, so one spawned during play belongs to its spawner.
	static ECrowdyOwnedBy ResolveOwnedBy(ECrowdyOwnedBy InOwnedBy, bool bIsLevelPlaced);

	// The role and owner id for a resolved owner: Player is this client's; World is WorldOwned, written by no client; Leader
	// waits for the current Leader as its owner.
	static void DeriveOwner(ECrowdyOwnedBy InResolvedOwnedBy, const FGuid& InLocalPlayerID, ECrowdyRole& OutRole, FGuid& OutWriterID);

	// Whether an entity with these settings sends its movement; bIsPawnWithMovement is IsPawnWithMovement of its class.
	static bool ResolveReplicatesMovement(ECrowdyReplicateMovement InReplicateMovement, bool bIsPawnWithMovement);

	// Whether ActorClass is a pawn with a movement component, Blueprint-added ones included. It walks the class's
	// component templates, so resolve it once per class rather than per entity.
	static bool IsPawnWithMovement(TSubclassOf<AActor> ActorClass);

	// The engine's per-placement identity for Actor, or an invalid guid when it has none. This is the Stable
	// identity seed. It is not a test for "was this actor placed in the level": the guid is editor-only data, and
	// in the editor a runtime-spawned actor is given one too. AActor::IsNetStartupActor answers that question.
	static FGuid ResolveActorInstanceGuid(const AActor* Actor);

	// Pure ownership test behind UCrowdyUtilities::DoesCrowdyEntityOwn: the target's WriterID is the owner's NetID.
	// A World entity (WorldOwned) on either side never matches.
	static bool DoesOwnershipMatch(ECrowdyRole OwnerRole, const FGuid& OwnerNetID,
		ECrowdyRole TargetRole, const FGuid& TargetWriterID);

	// Which PlayerDerived condition decided this entity's identity, from the facts read at the decision point.
	// Reports the FIRST unmet condition, so a bare actor is answered "not a Pawn" and never "no game session":
	// the later questions are not asked of an actor the earlier ones already rule out.
	static ECrowdyPlayerDerivedIdentity ClassifyPlayerDerivedIdentity(const FCrowdyPlayerDerivedIdentityFacts& Facts);

	// The clause naming that verdict in a diagnostic, phrased as the condition that was actually measured.
	static const TCHAR* DescribePlayerDerivedIdentity(ECrowdyPlayerDerivedIdentity Verdict);

	// Whether a self-resolving Player Derived pawn waits for its first possession: a pawn spawned during play has
	// no controller at BeginPlay, and reading the session there would mint a random id that is not the session's.
	static bool ShouldDeferIdentityToPossession(bool bInjected, ECrowdyIdentityPolicy Policy, bool bIsPawn, bool bHasController);

	// Whether an entity that replicates movement, with Auto Register ticked, ends BeginPlay without joining the
	// continuous state channel because it is a World entity. Auto Register starts the channel only for an entity this
	// client owns, and no client writes a World entity, so it leaves BeginPlay sending nothing at all.
	static bool ShouldReportHostOwnedAutoRegister(bool bInReplicatesMovement, bool bInAutoRegister, ECrowdyRole InRole);

	uint32 GetClassID() const { return ClassID; }

	// Whether the engine's placement guid may be used as this entity's shared id. It may only when the actor was
	// loaded with its level AND the engine actually has a guid for it. The level-placed half is what makes the id
	// shared: the guid is saved in the level, so every client reads the same one. An actor created during play has
	// no such shared origin, and asking the engine for its guid answers with a fresh per-run, per-client value in
	// the editor and with nothing at all in a packaged build, so honouring it would make the two disagree.
	static bool ShouldUsePlacementGuid(bool bIsLevelPlaced, bool bHasInstanceGuid);

	// The snapshot taken at registration wins over a live read. A cooked build releases the guid after the actor
	// registers its components, and a live read inside a level instance then answers a valid but shared value
	// (the level's guid combined with nothing), so the live value is only a fallback for a component that
	// registered too late to snapshot. In an editor build both are the same value.
	static FGuid SelectPlacementGuid(const FGuid& AtRegister, const FGuid& Live);

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

	// Injects the entity subsystem and game session that BeginPlay and ResolveIdentity would look up from the
	// world, so a headless test in an editor world (which creates neither) drives the real registration path.
	void SetCollaboratorsForTest(UCrowdyEntitySubsystem* InEntities, UCrowdyGameSession* InSession);

	// How many times each diagnostic has spoken on this instance, and the last thing each said.
	int32 PlayerDerivedFallbackReportCount = 0;
	int32 HostOwnedAutoRegisterReportCount = 0;
	FString LastPlayerDerivedFallbackMessage;
	FString LastHostOwnedAutoRegisterMessage;

	// How many times this component has resolved its identity and registered.
	int32 IdentityResolutionCount = 0;

	// How many times OnCrowdyOwnershipAssigned has been announced, and the values carried by the last one.
	int32 OwnershipAnnouncementCount = 0;
	FGuid LastAnnouncedWriterID;
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
	virtual void SendPresence() const override;

	// Component config settable from an owning actor's constructor;
	// EditAnywhere keeps them tweakable on instances and Blueprint defaults.

	// Whether this entity sends its movement every replication interval, through the StateExecutor. Auto: on for a pawn
	// with a movement component. Unrelated to ownership and to CrowdyState property replication, which work either way.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Entity Component",
		meta=(ToolTip="Whether this entity sends its movement every replication interval. Auto: on for a pawn with a movement component, off for anything else."))
	ECrowdyReplicateMovement ReplicateMovement = ECrowdyReplicateMovement::Auto;

	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component")
	ECrowdyIdentityPolicy IdentityPolicy = ECrowdyIdentityPolicy::Stable;

	// Who owns this entity: the client that writes its view state. Consumed only on the self-resolving identity path;
	// injected-identity spawns pass an explicit role and ignore it.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy SDK|Entity Component",
		meta=(ToolTip="Who owns this entity: the client that writes its view state. Auto: World when placed in the level, Player when spawned during play."))
	ECrowdyOwnedBy OwnedBy = ECrowdyOwnedBy::Auto;

	// Per-entity CrowdyState keyframe-heartbeat control. Inherit (default): this entity's CrowdyHeartbeat-marked
	// properties ride the periodic heartbeat when the map enables it. Off: this entity never emits a keyframe
	// heartbeat, a kill switch for always-active entities. On-change replication is unaffected either way; the
	// per-property CrowdyHeartbeat opt-in still decides which properties a heartbeat would carry.
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component")
	ECrowdyStateHeartbeat StateHeartbeat = ECrowdyStateHeartbeat::Inherit;

	// When true, this entity's current authority (its owning client) grants
	// any ownership-transfer request immediately, instead of surfacing UCrowdyEntitySubsystem::OnOwnershipRequested
	// for game code to decide. Authored config, read on whichever client is the authority when a request arrives.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Crowdy SDK|Entity Component")
	bool bAutoApproveOwnershipRequests = false;

	// Produces the snapshot the AutoReplicator sends, for an entity that replicates movement.
	UPROPERTY(EditAnywhere, Instanced, Category="Crowdy SDK|Entity Component",
		meta=(EditCondition="ReplicateMovement != ECrowdyReplicateMovement::Off", EditConditionHides))
	TObjectPtr<UActorUpdateExecutor> StateExecutor;

	// Join the AutoReplicator on BeginPlay, for an entity that replicates movement; call StartReplication otherwise.
	UPROPERTY(EditAnywhere, Category="Crowdy SDK|Entity Component",
		meta=(EditCondition="ReplicateMovement != ECrowdyReplicateMovement::Off", EditConditionHides))
	bool bAutoRegister = true;

protected:

	virtual void OnRegister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	UPROPERTY()
	FGuid NetID;

	// The owner's placement guid, taken in OnRegister. A cooked build keeps that guid only from load until the
	// actor's PostRegisterAllComponents releases it, which is before BeginPlay, so reading it there finds nothing.
	FGuid PlacementGuidAtRegister;

	UPROPERTY()
	FGuid WriterID;

	// The injected owner user, carried into the record at registration; the record holds the current value after that.
	UPROPERTY()
	int64 OwnerUserID = 0;

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

	// Set when Player Derived identity resolved to the session's id, so the record names this pawn as the player's avatar.
	bool bResolvedAsAvatar = false;

	// Set by MarkPooledDormant before BeginPlay and cleared when the pool assigns a real identity. While it is
	// set the component registers nothing, so no listener can act on an actor that stands for no entity.
	bool bPooledDormant = false;

	bool bStateExecutorPrepared = false;

	// True while a Player Derived pawn that began play unpossessed waits for its first controller.
	bool bAwaitingPossession = false;

	// Set by EnablePresence on an entity spawned with Spawn Entity; cleared with the identity.
	bool bSendsPresence = false;

	// True once OnCrowdySpawned has fired for this identity; cleared with it so a pooled actor gets the next one's.
	bool bSpawnDataDelivered = false;

	// The InitialState OnCrowdySpawned delivered, kept so a presence send repeats it for players who join later.
	UPROPERTY(Transient)
	FInstancedStruct SpawnData;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakObjectPtr<UCrowdyEntitySubsystem> EntitySubsystemForTest;
	TWeakObjectPtr<UCrowdyGameSession> GameSessionForTest;
#endif

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

	UCrowdyEntitySubsystem* ResolveEntitySubsystem() const;
	UCrowdyGameSession* ResolveGameSession() const;

	void ResolveIdentity();

	// Resolves, registers and starts the mode's channel; from BeginPlay, or from a deferred pawn's first possession.
	void CompleteRegistration();

	// A pooled actor skips registration, so this also runs when one first takes ownership.
	void PrepareStateExecutor();

	// Joins the auto replicator as a presence entry, which calls SendPresence on a slow fixed interval.
	void StartPresence();

	// One client has one session id, so a newer Player Derived pawn takes the id from an earlier pawn still alive
	// after unpossession (a respawn that lets the old pawn expire); the collision guard would refuse it otherwise.
	void SupersedePreviousPlayerPawn();

	void StopAwaitingPossession();

	UFUNCTION()
	void HandlePawnControllerChanged(APawn* Pawn, AController* OldController, AController* NewController);

	// Says that PlayerDerived identity fell back to a random id, which condition caused it, and what to set
	// instead. Speaks once per component instance.
	void ReportPlayerDerivedFallback(ECrowdyPlayerDerivedIdentity Verdict);

	// Says that a World entity finished BeginPlay without joining the continuous state channel despite Auto
	// Register being ticked, and what to set instead. Speaks once per component instance.
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
	// this component's cached WriterID/Role and, for an entity that replicates movement, moves the continuous
	// StateExecutor channel to follow authority (the new owner joins the AutoReplicator; a client that lost ownership
	// leaves it). A spawned entity's presence sends follow authority the same way.
	// It does not announce; the subsystem does that afterwards, via BroadcastOwnershipAssigned.
	void ApplyOwnershipReassignment(const FGuid& NewWriterID, ECrowdyRole NewRole);

	// The local player's actor id changed, and a Player Derived pawn's id is that actor id: stops sending under the old
	// one and takes NewID. True when it was sending, so ResumeAfterRekey starts it again, with OutExtras, once it is
	// registered anew.
	bool StopForRekey(const FGuid& NewID, TArray<uint8>& OutExtras);
	void ResumeAfterRekey(bool bWasSending, TArray<uint8> Extras);

	// The local player's actor id changed and this entity is still theirs: it sends under NewWriterID's wire id from
	// here, keeping its update extras, and announces nothing.
	void RepointForRekey(const FGuid& NewWriterID);
	void RestoreExtras(TArray<uint8> Extras);
};
