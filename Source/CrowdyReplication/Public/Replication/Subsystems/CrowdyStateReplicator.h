#pragma once

#include "CoreMinimal.h"
#include "Containers/BitArray.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "GameFramework/Actor.h"
#include "Replication/State/FCrowdyStateDelta.h"
#include "Subsystems/WorldSubsystem.h"
#include "CrowdyStateReplicator.generated.h"

class UCrowdyAutoRegistry;
class UCrowdyEntitySubsystem;
class UCrowdyGameSession;
struct FCrowdyRepLayout;
struct FCrowdyEntityRecord;

/**
 * Internal per-owned-entity send state for the CrowdyState replicator. Deliberately NOT a USTRUCT: it
 * holds raw FProperty pointers plus a void shadow buffer that have no wire form and are never serialized.
 *
 * Shadow representation is Approach A (the decision recorded in FCrowdyRepLayout.h): a parallel value
 * buffer laid out by the layout's own property slots, one InitializeValue'd slot per property. Per tick the
 * diff is FProperty::Identical(live, shadow); the post-send update is FProperty::CopyCompleteValue(shadow,
 * live); teardown is FProperty::DestroyValue(shadow) in the destructor.
 *
 * INVARIANT (load-bearing): ShadowData is sized exactly once in InitShadow and NEVER resized, so
 * ShadowData.GetData() is a stable base for the entity's whole lifetime. The whole FCrowdyOwnedEntityState
 * is itself pinned behind a TUniquePtr in the OwnedEntities map, so neither the buffer nor its base pointer
 * moves when the map rehashes on inserts/removes of other entities.
 *
 * LAYOUT LIFETIME: the registry frees and rebuilds its FCrowdyRepLayout objects on any rescan (a world
 * init, a Live Coding reload), so this struct deliberately does NOT cache the layout pointer. The
 * replicator re-resolves the live layout every tick via FindRepLayout (a cheap cache hit); ShadowProps
 * snapshots the FProperty* the shadow was built with so teardown never dereferences a freed layout. Those
 * FProperty* are owned by the UClass and survive a rescan; only a structural Live Coding reload (which
 * reinstances the class) invalidates them, and HandleReloadComplete abandons the shadow of any entity whose
 * actor was reinstanced before that stale pointer can be dereferenced.
 */
struct FCrowdyOwnedEntityState
{
	FGuid EntityID;

	// The diff container AND, for a spatial participant, the dispatch context/target. Any UObject can be a
	// replicated participant (an actor, or a non-actor UObject such as a subsystem), so this is widened from
	// the actor-only weak pointer it replaces. Weak so a dead participant is detected, never kept alive.
	// Shadow/layout resolution needs only GetClass(); GetActor() is used only where an ACTOR is genuinely
	// required (spatial dispatch location) and is null for a non-actor participant.
	TWeakObjectPtr<UObject> Participant;

	AActor* GetActor() const { return Cast<AActor>(Participant.Get()); }

	// True for a non-spatial (non-actor) participant, e.g. a host-owned subsystem: it has no world location,
	// so its deltas ride the reliable channel transport (PublishReliableRpc / EncodeChannelStateDelta) rather
	// than the spatial DispatchGameEvent, and it never emits an owner-only targeted delta (a singleton has no
	// per-owner target). Auto-diff still runs for it (unlike a spatial host-owned world actor).
	bool bNonSpatial = false;

	// Hash of the layout the shadow was built against. The replicator re-resolves the live layout every
	// tick (a cached pointer would dangle when the registry frees its layouts on a rescan); if the
	// re-resolved hash differs, the property set changed and the shadow is rebuilt before it is used.
	int64 ShadowLayoutHash = 0;

	// One aligned slot per property, InitializeValue'd once in InitShadow. Sized once, never resized.
	TArray<uint8> ShadowData;

	// Byte offset of each slot into ShadowData. Parallel to ShadowProps.
	TArray<int32> ShadowOffsets;

	// The FProperty each slot holds, snapshotted at InitShadow so teardown never dereferences the registry's
	// (possibly-freed) layout. Owned by the UClass, so they survive a registry rescan; a structural Live
	// Coding reload that reinstances the class is handled by HandleReloadComplete.
	TArray<const FProperty*> ShadowProps;

	// True once InitShadow has run to completion. Gates the destructor's DestroyValue pass so a never-built
	// or moved-from state tears down safely.
	bool bShadowInitialized = false;

	// True when this entry is a HostOwned (world/AI) entity tracked because the local client is the host.
	// Host-owned entities are NEVER auto-diffed (their auto-diff pass yields no dirty bits): only their
	// explicitly marked (PendingManualDirty) properties emit, plus the keyframe heartbeat so a late observer
	// still receives the world entity's baseline. An Owner entity leaves this false.
	bool bHostOwned = false;

	// True when this entity's UCrowdyEntityComponent set StateHeartbeat == Off (a per-entity kill switch), read
	// once in BuildOwnedState. Suppresses this entity's keyframe heartbeat only; on-change replication is
	// unaffected. False when the entity has no component (e.g. test fixtures) or set Inherit.
	bool bHeartbeatSuppressed = false;

	// Per-property push flags for bManualDirty properties: set by MarkStateDirty/MarkAllStateDirty, consumed
	// (and cleared for the sent bits) by the send loop. Sized to the layout in InitShadow and re-Init on a
	// shadow rebuild. A bManualDirty property is dirty iff its bit here is set; auto-diff never touches it.
	TBitArray<> PendingManualDirty;

	// World-time (seconds) of this entity's next keyframe heartbeat. Seeded in BuildOwnedState to about one
	// interval out with a small deterministic per-entity stagger so heartbeats do not synchronize into a burst.
	double NextKeyframeTime = 0.0;

	// Total blob bytes emitted for this entity on its most recent send tick; 0 until the first send. Diagnostic only.
	int32 LastSentBytes = 0;

	FCrowdyOwnedEntityState() = default;

	// Non-copyable: it owns InitializeValue'd slots (FString/USTRUCT heap) that must be DestroyValue'd
	// exactly once. Copying would double-free; the map stores it behind TUniquePtr, so it is only ever
	// moved/pinned. Move members stay defaulted so TUniquePtr/TMap still work.
	FCrowdyOwnedEntityState(const FCrowdyOwnedEntityState&) = delete;
	FCrowdyOwnedEntityState& operator=(const FCrowdyOwnedEntityState&) = delete;
	FCrowdyOwnedEntityState(FCrowdyOwnedEntityState&&) = default;
	FCrowdyOwnedEntityState& operator=(FCrowdyOwnedEntityState&&) = default;

	// Out-of-line in the .cpp (needs the full FProperty/FCrowdyRepLayout definitions to call DestroyValue).
	~FCrowdyOwnedEntityState();
};

/**
 * CrowdyState send plane. At the map cadence (ReplicationIntervalHz) it diffs the entities this client
 * drives against each entity's shadow buffer, encodes the changed properties via the Phase 2 codec, and
 * dispatches an FCrowdyStateDelta. Phase 5 adds authority + targeting on top of the Phase 3 spatial diff:
 *   - Owner-only properties (bOwnerOnly) are auto-diffed like spatial ones but shipped as a SECOND,
 *     targeted (single-actor) delta so only the owning client receives them; spatial properties still
 *     broadcast. Both deltas share the same LayoutHash (positional selector over the full layout).
 *   - Manual-dirty properties (bManualDirty) are never auto-diffed; they emit only when MarkStateDirty /
 *     MarkAllStateDirty flags them, routed to the spatial or targeted delta by their own bOwnerOnly flag.
 *   - When the local client is the elected host, every emitted delta is stamped HostSourced (a
 *     precedence-by-convention hint the receiver honors only for entities it owns).
 *   - HostOwned (world/AI) entities are tracked too, but only when the local client is host, and only
 *     their manual-dirty bits + keyframe heartbeat emit (auto-diff off).
 *   - Each entity emits a periodic keyframe heartbeat (StateKeyframeIntervalSeconds) carrying every
 *     non-owner-only property, so a late observer gets a baseline without waiting for a change. There is no
 *     relevance-gain trigger, so a peer that becomes relevant just after a keyframe (with no prior delta for
 *     the entity) may hold stale/default values for up to one full interval until the next heartbeat.
 * The receive/apply side (echo-drop, foreign-non-host drop for owned entities, decode, OnRep, host-value
 * adoption) lives in UCrowdyEventRouter (Phase 4/5).
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyStateReplicator : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual TStatId GetStatId() const override;
	virtual void Tick(float DeltaTime) override;

	// Marks one bManualDirty property of an owned entity for a push on the next send. No-op (verbose log) if
	// the entity is not tracked or the named property is not a manual-dirty property of its layout.
	void MarkStateDirty(const FGuid& EntityID, FName PropertyName);

	// Marks every bManualDirty property of an owned entity for a push on the next send.
	void MarkAllStateDirty(const FGuid& EntityID);

	// Adopts a host correction that was just applied to an entity we own into the owner's shadow, so the
	// owner's next diff sees the host's values as already-sent and does not re-emit (revert) them. Called by
	// the router after it decodes a HostSourced delta for a locally-owned entity. Best-effort: a null / not-
	// tracked / layout-mismatched entity is silently skipped.
	void AdoptHostValues(const FGuid& EntityID, const FCrowdyRepLayout& Layout,
		const TArray<int32>& ChangedIndices, const void* AppliedContainer);

	// Read-only diagnostic: true iff Actor is non-null and its class carries at least one CrowdyState property
	// (i.e. AutoRegistry->FindRepLayout returns a valid layout). It reports only whether the class participates
	// in CrowdyState, NOT whether this client currently drives the actor. Null-safe (false for a null actor or
	// an unavailable registry).
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|State")
	bool IsStateReplicated(const AActor* Actor) const;

	// Read-only diagnostic: the total blob bytes emitted for Actor on its most recent send tick, or -1 if Actor
	// is not an owned entity driven by this client ("not driven by this client"). The value is an in-editor
	// diagnostic of the last local send, NOT a wire guarantee: it reflects what THIS client last emitted, is 0
	// until the first send, and says nothing about what any peer received.
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Crowdy SDK|State")
	int32 GetLastSentStateBytes(const AActor* Actor) const;

	// True when crowdy.state.loopback is set. When on, a sent CrowdyState delta for an owned entity is
	// also decoded onto a local "mirror" entity (a lazily-spawned, distinct instance of the same class,
	// RemoteProxy role, its own NetID) so decode/OnRep/host-authority checks can be exercised from a single
	// PIE client. Off (default) is byte-identical to today: no new code runs on any ReplicationLoop path.
	static bool IsLoopbackEnabled();

	// Test seams (headless tests need no world/bridge). When DispatchHookForTests is bound, ReplicationLoop
	// routes every outgoing SPATIAL (actor) delta here INSTEAD of the real transport (bTargeted flags a
	// single-actor owner-only delivery vs a broadcast).
	TFunction<void(const FCrowdyStateDelta& /*Delta*/, bool /*bTargeted*/)> DispatchHookForTests;

	// When bound, ReplicationLoop routes a NON-SPATIAL (subsystem) participant's channel delta here INSTEAD of
	// encoding + PublishReliableRpc, so a headless test can distinguish a channel dispatch from a spatial one
	// (an actor delta hits DispatchHookForTests, a subsystem delta hits this).
	TFunction<void(const FCrowdyStateDelta& /*Delta*/)> ChannelDispatchHookForTests;

	// Out-of-line in the .cpp: TWeakObjectPtr<UCrowdyAutoRegistry>::operator= needs the complete type, and
	// UCrowdyAutoRegistry is only forward-declared here (its full header is a .cpp-only include).
	void SetRegistryForTest(UCrowdyAutoRegistry* InRegistry);
	void SetLocalPlayerIDForTest(const FGuid& InID) { LocalPlayerID = InID; }

	// Overrides the host id (else GetHostID) and the loop clock (else world time), so a headless replicator
	// with no world/session can prove host-precedence and keyframe timing deterministically.
	void SetHostIDForTest(const FGuid& InID) { HostIDForTest = InID; bUseHostOverrideForTest = true; }
	void SetTimeForTest(double InSeconds) { TestTimeSeconds = InSeconds; bUseTimeOverrideForTest = true; }
	void SetKeyframeIntervalForTest(float InSeconds) { KeyframeInterval = InSeconds; }

	// Adds an owned participant directly, bypassing the OnEntityRegistered subscription. Resolves the layout via
	// AutoRegistry->FindRepLayout(Participant->GetClass()); returns false (adds nothing) if there is none. Takes
	// UObject* so a non-actor (subsystem) participant can be registered too; an AActor* caller still binds via
	// the standard actor->UObject conversion.
	bool RegisterOwnedEntityForTest(const FGuid& EntityID, UObject* Participant);

	// Drives the ownership gate the way HandleEntityRegistered does, but from a caller-supplied record so a
	// headless test can prove a non-owner record is rejected without a live entity subsystem.
	bool TryTrackOwnedForTest(const FCrowdyEntityRecord& Record) { return TryTrackOwned(Record); }
	void RunReplicationLoopForTest() { ReplicationLoop(); }
	int32 NumOwnedForTest() const { return OwnedEntities.Num(); }

	// Host-lifecycle test seams (headless tests have no live world/session). SetEntitySubsystemForTest injects a
	// real entity subsystem so FindRecord/GetLocalPlayerID resolve; the Handle*ForTest wrappers drive the private
	// registration and host-change paths from a test.
	void SetEntitySubsystemForTest(UCrowdyEntitySubsystem* InES);
	void HandleEntityRegisteredForTest(const FGuid& NetID) { HandleEntityRegistered(NetID); }
	void HandleHostChangedForTest(const FGuid& NewHostID, const FGuid& PreviousHostID) { HandleHostChanged(NewHostID, PreviousHostID); }
	void HandleOwnershipChangedForTest(AActor* TargetEntity, const FGuid& NetID, const FGuid& NewOwnerID, const FGuid& PreviousOwnerID)
	{
		HandleOwnershipChanged(TargetEntity, NetID, NewOwnerID, PreviousOwnerID);
	}
	int32 NumKnownHostOwnedForTest() const { return KnownHostOwnedIDs.Num(); }
	bool IsTrackedForTest(const FGuid& Id) const { return OwnedEntities.Contains(Id); }

	// Loopback mirror test seams: exercise the spawn/guard logic directly without ReplicationLoop/Tick.
	AActor* GetOrCreateLoopbackMirrorForTest(AActor* SourceActor, const FGuid& SourceEntityID)
	{
		return GetOrCreateLoopbackMirror(SourceActor, SourceEntityID);
	}
	int32 NumLoopbackMirrorsForTest() const { return LoopbackMirrors.Num(); }

private:

	void ReplicationLoop();

	// Drains PendingHostPushes: for each queued one-shot host push, encode the property's CURRENT live value and
	// broadcast it (HostSourced iff we are host), with no shadow and no persistent tracking. Called first in
	// ReplicationLoop so a push fires even when this client tracks no owned entities.
	void DrainPendingHostPushes();

	bool BuildOwnedState(const FGuid& EntityID, UObject* Participant, TUniquePtr<FCrowdyOwnedEntityState>& Out);
	void InitShadow(FCrowdyOwnedEntityState& State, const FCrowdyRepLayout& Layout);

	// Loopback (crowdy.state.loopback): returns the existing mirror for SourceEntityID, or spawns one. The
	// mirror is a second instance of SourceActor's class, in the same world, registered with its OWN fresh
	// NetID as ECrowdyRole::RemoteProxy and an INVALID OwnerID (never SourceActor's owner, and never
	// LocalPlayerID: UCrowdyEntitySubsystem::IsLocallyOwned checks OwnerID, not Role, so an owner-stamped
	// mirror would still trip DispatchStateDelta's owned-entity gate and TryTrackOwned would track it as a
	// second owned entity, mirroring the mirror). Built with a bespoke, purely local SpawnActorDeferred ->
	// InitIdentity -> FinishSpawning sequence never UCrowdyEntitySubsystem::SpawnEntity, which hardcodes
	// Role::Owner + OwnerID=LocalPlayerID and also broadcasts a spawn announcement over the real transport.
	// Returns null (and destroys the half-spawned actor) if SourceActor's class carries no
	// UCrowdyEntityComponent, or if the component failed to self-register after FinishSpawning: either way
	// DispatchStateDelta could never resolve a delta targeted at it, which would otherwise silently burn the
	// full deferred-retry budget every time. Idempotent: a cached hit skips spawning entirely.
	AActor* GetOrCreateLoopbackMirror(AActor* SourceActor, const FGuid& SourceEntityID);

	// Destroys and forgets one entity's loopback mirror, if any. Called from HandleEntityUnregistered (the
	// real entity went away) and Deinitialize (world teardown).
	void DestroyLoopbackMirror(const FGuid& SourceEntityID);

	// The ownership gate shared by the delegate handler and the test seam: tracks a record this client drives.
	// An ECrowdyRole::Owner record is always tracked; an ECrowdyRole::HostOwned record is tracked ONLY when the
	// local client is the host (a non-host receives world-entity state as a proxy, never simulates it). The
	// entity must have a live actor with a rep layout. Returns true when a new owned entry was added.
	bool TryTrackOwned(const FCrowdyEntityRecord& Record);

	// Host resolution (test-overridable). ResolveHostID returns the injected id when set, else the session's
	// host id; IsLocalHost is true when that host id is valid and equals the local player id (read live).
	FGuid ResolveHostID() const;
	bool IsLocalHost() const;

	// Loop clock (test-overridable): the injected time when set, else the world time. A headless replicator has
	// no world, so the override drives keyframe timing deterministically.
	double GetLoopTimeSeconds() const;

	UFUNCTION()
	void HandleEntityRegistered(const FGuid& NetID);

	UFUNCTION()
	void HandleEntityUnregistered(const FGuid& NetID);

	// Bound to the game session's OnHostIDUpdated (fired only on an actual host change). When we become the host,
	// promote every known host-owned entity to tracked; when we lose the host, drop all of them. The local id is
	// read live and this runs on the game thread. This is the mid-session and post-travel host-change path; the
	// already-elected-on-arrival case is handled by TryTrackOwned reading GetHostID() live at registration.
	UFUNCTION()
	void HandleHostChanged(const FGuid& NewHostID, const FGuid& PreviousHostID);

	// Bound to the entity subsystem's OnEntityOwnershipChanged (an explicit runtime ownership transfer). Drops any
	// stale tracking/shadow for the entity, re-maintains KnownHostOwnedIDs from the updated record (so a later host
	// migration still promotes/demotes it), and re-tracks the entity when this client is now its authority. The
	// per-entity analogue of HandleHostChanged; the record is already re-pointed by ReassignOwnership when this runs.
	UFUNCTION()
	void HandleOwnershipChanged(AActor* TargetEntity, const FGuid& NetID, const FGuid& NewOwnerID, const FGuid& PreviousOwnerID);

#if WITH_EDITOR
	// A Live Coding reload rebuilds the registry's layouts and may reinstance replicated actor classes. Any
	// owned entity whose actor was reinstanced now holds a shadow whose snapshotted FProperty* are stale, so
	// abandon those shadows (skip DestroyValue) before a tick can dereference them; survivors keep ticking
	// against the freshly re-resolved layout.
	void HandleReloadComplete();
	FDelegateHandle ReloadCompleteHandle;
#endif

	// Owned entities keyed by NetID; TUniquePtr pins the shadow buffer (no move on rehash).
	TMap<FGuid, TUniquePtr<FCrowdyOwnedEntityState>> OwnedEntities;

	// Loopback (crowdy.state.loopback) mirror actors, keyed by the REAL owned entity's NetID. Populated
	// lazily by GetOrCreateLoopbackMirror; empty whenever the CVar has never been on this session.
	TMap<FGuid, TWeakObjectPtr<AActor>> LoopbackMirrors;

	// A one-shot host super-user push queued by MarkStateDirty for an entity this client does NOT track (e.g. the
	// host overriding another client's owned entity). Drained once by ReplicationLoop: the CURRENT live value of the
	// property is encoded and broadcast (HostSourced iff we are host), with no shadow and no persistent tracking.
	// Unlike the tracked manual-dirty path this does NOT require bManualDirty (a host override may target any
	// CrowdyState property).
	struct FCrowdyPendingHostPush
	{
		FGuid EntityID;
		int32 PropertyIndex = INDEX_NONE;
		bool operator==(const FCrowdyPendingHostPush& Other) const
		{
			return EntityID == Other.EntityID && PropertyIndex == Other.PropertyIndex;
		}
	};

	TArray<FCrowdyPendingHostPush> PendingHostPushes;

	// Every registered HostOwned entity id, maintained regardless of local host status: a non-host still records
	// the id so a later promotion (HandleHostChanged) can start tracking it, and a demotion can stop tracking it.
	TSet<FGuid> KnownHostOwnedIDs;

	TWeakObjectPtr<UCrowdyAutoRegistry> AutoRegistry;
	TWeakObjectPtr<UCrowdyEntitySubsystem> EntitySubsystem;

	// The game-instance-scoped session (survives level travel); source of the host-change delegate. Weak so a torn-
	// down session is detected rather than kept alive.
	TWeakObjectPtr<UCrowdyGameSession> GameSession;

	FGuid LocalPlayerID;
	float ReplicationInterval = 0.1f;
	float ReplicationAccumulator = 0.f;
	ECrowdyReplicationDistance RelevanceDistance = ECrowdyReplicationDistance::Four_Chunks;
	bool bIsTicking = false;

	// Seconds between per-entity keyframe heartbeats; set from the profile in Initialize (clamped >= 0.1).
	// Defaults to 2.0 so the hook test path (which never runs Initialize) still has a sane interval.
	float KeyframeInterval = 2.0f;

	// Host / time overrides for headless tests (see the Set*ForTest seams). Off in production.
	FGuid HostIDForTest;
	bool bUseHostOverrideForTest = false;
	double TestTimeSeconds = 0.0;
	bool bUseTimeOverrideForTest = false;
};
