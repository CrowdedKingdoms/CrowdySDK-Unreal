// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Hash/CityHash.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/UDP/Enums/ECrowdyMessageType.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "Network/GraphQL/FCrowdyGameApiCodec.h"
#include "Replication/GameModel/CrowdyContainerStandIn.h"
#include "Replication/GameModel/CrowdyGameModelSessionTypes.h"
#include "Replication/GameModel/CrowdyModelNotificationSink.h"
#include "Replication/Subsystems/ICrowdyEntitySubscriber.h"
#include "CrowdyGameModelSubsystem.generated.h"

class UCrowdyEntitySubsystem;
class UCrowdyEventRouter;
class UCrowdyGameSession;
class UCrowdySDKBridgeSubsystem;
class FCrowdyCppClient;
class FCrowdyServiceRegistry;
struct FServerEventNotification;
struct FChannelMessageNotification;
struct FCrowdyAttributeChange;
struct FCrowdyModelChangedPing;
struct FCrowdyDelivery;
struct FCrowdyCppJsonResult;

// Fires on the game thread when a free/data container's cached state changes after a pull, the OnRep analogue
// for actorless containers (inventories, quests). A UI binds this and filters by ContainerId. Actor-bound
// containers fire their actor's CrowdyOnRep instead; this delegate is only for containers with no entity.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdyDataContainerChanged, const FString&, ContainerId);

/**
 * A signal arrived. SignalName is the name authored on the effect (without the OnSignal_ prefix), ContainerId names
 * the container it was fired for, and Target is the locally bound object for that container, or null when this
 * client has nothing bound to it. A signal carries no state of its own: read whatever you need off the container.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FCrowdySignalReceived, const FString&, SignalName,
	UObject*, Target, const FString&, ContainerId);

// Fires on the game thread once per attribute whose value changed on any apply path (a re-pull, a confirmed
// invoke, or a free/data-container refresh). Target is the bound actor/object, or null for a free/data
// container addressed only by id; ModelId is the server container id; Attribute is the server key;
// OldValueJson/NewValueJson are canonical JSON, with NewValueJson empty when the attribute was removed. This is
// the zero-setup change observer: bind once (optionally filtered) to watch every model change without a
// per-attribute OnRep and without knowing which container or notification carrier delivered it. Per-object
// reactions still use the attribute's CrowdyOnRep; this delegate is the cross-object/UI channel.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FCrowdyModelAttributeChanged, UObject*, Target,
	const FString&, ModelId, FName, Attribute, const FString&, OldValueJson, const FString&, NewValueJson);

// Fires on the game thread once per session change this client hears about, from either carrier: the channel cue
// (no payload) or a watched session's event stream (full event). Filter by SessionId; see OnSessionChanged.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FCrowdyOnSessionChanged, FCrowdyGameModelSessionEvent, Event);

namespace CrowdyCoalesce
{
	// The 64-bit content hash every string-valued part of a merge key is reduced to, so deciding whether two applies
	// are the same call costs no string building and no allocation on a path that runs many times a second. Two
	// different strings that collided here would let two applies merge that must not; at any plausible number of open
	// windows that is far rarer than the round-off already accepted on the summed magnitude, and the alternative is
	// rebuilding and re-hashing a several-hundred-byte string on every apply. Defined once, inline, because two
	// translation units of this module both need it and a unity build merges duplicate helpers into one redefinition.
	inline uint64 HashString(const FString& Value)
	{
		return CityHash64(reinterpret_cast<const char*>(*Value), static_cast<uint32>(Value.Len() * sizeof(TCHAR)));
	}

	// Fold one more 64-bit component into a running hash. The engine's HashCombine is 32-bit; halving the width of a
	// key that decides whether two writes are the same call is not worth the saving. Order matters, so a caller that
	// needs an order-independent fold (a map's entries) sums the per-entry results of this instead of chaining it.
	inline uint64 MixHash(uint64 Accumulated, uint64 Value)
	{
		return Accumulated ^ (Value + 0x9E3779B97F4A7C15ULL + (Accumulated << 6) + (Accumulated >> 2));
	}
}

/**
 * One invoke offered to the coalescer: an effect apply that is willing to be merged with other applies of the same
 * effect to the same target, so a burst of them costs one gameModelInvoke instead of one each.
 *
 * Exactly one route is used. An entity-bound apply names SelfNetID and leaves ContainerId empty; a free/data apply
 * names ContainerId. Which one it is comes from bEntityBound rather than from an empty string, so an empty container
 * id is a failure the invoke reports and never a silent switch to the other route.
 *
 * A request that cannot actually be merged (no window, no summable parameter, the coalescer at capacity) is still
 * accepted and simply dispatched on its own: the caller's completion runs either way and never has to know which
 * happened.
 */
struct FCrowdyCoalesceRequest
{
	bool bEntityBound = false;
	FGuid SelfNetID;
	FString ContainerId;

	FString FunctionName;
	FString SessionId;

	// The marshalled invoke parameters. A merged window TAKES this object over and writes the summed value into it
	// when the window closes, so pass one the caller does not keep a reference to; the effect marshaller builds a
	// fresh one per apply, which is exactly that.
	TSharedPtr<FJsonObject> Params;

	// The parameter repeated applies sum into. It must be present in Params as a JSON number; empty, missing or any
	// other JSON type means this apply is not merged.
	FString AccumulateParam;

	// Everything besides the target, the function and the session that two applies must agree on before they may be
	// summed, as an opaque hash the caller builds. Two applies whose discriminators differ never merge, so the caller
	// decides what "the same call" means; the coalescer only ever compares. It is built from what DETERMINES the
	// parameters rather than read back off the parameter object, so it never depends on how a JSON object happens to
	// enumerate its keys.
	uint64 MergeDiscriminator = 0;

	// Whether the accumulated parameter is declared as an int, so the summed value is rounded to a whole number
	// before it is sent rather than reaching the server as a fraction the schema does not accept.
	bool bAccumulateIsInteger = false;

	// The author's merge window in seconds, before the governor stretches it. Zero or less means no merging.
	float WindowSeconds = 0.0f;
};

/**
 * Runtime home for the Game Model plane: the server-authoritative source of truth for gameplay state. Two roles:
 *
 *  1. Transport facade: supplies the Game API endpoint and the app-scoped bearer token + app id so gameplay
 *     code passes only gameplay-relevant fields, then marshals each call through FCrowdyGameApiCodec.
 *     appId is always the session's (never the caller's) so it matches the token's app scope. Fails loudly on
 *     the game thread when the endpoint or token is missing rather than dropping the call.
 *
 *  2. Container cache and OnRep: binds an entity's realtime NetID to a server container id, caches the
 *     container's last-seen property values, and fires each changed attribute's parameterless CrowdyOnRep on the
 *     bound actor when a re-pull or a confirmed invoke changes a value. State is always pulled, never pushed. A
 *     model-changed notification (the fallback ping, or the server-native event) funnels into HandleModelChanged,
 *     which re-pulls and applies.
 *
 * Attribute discovery reads live UPROPERTY metadata in the editor and a baked table in a cooked build. Bindings
 * are supplied explicitly via BindEntityContainer, or resolved automatically for a CrowdyContainer-tagged entity.
 */
UCLASS(BlueprintType)
class CROWDYREPLICATION_API UCrowdyGameModelSubsystem : public UWorldSubsystem, public ICrowdyModelNotificationSink
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	// Auto-register tagged subsystems once the world (and every other subsystem) is up. Runs after Initialize's
	// OnEntityRegistered subscription, so a fresh RegisterParticipant broadcast reaches HandleEntityRegistered.
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	// ICrowdyModelNotificationSink: the seam a consumer subscribes to. Every inbound carrier normalizes to one
	// FCrowdyModelChangeHint and broadcasts here (see NotifyModelChanged); the subsystem's own re-pull is bound
	// as the sole consumer today, but any observer can subscribe without knowing which carrier delivered.
	virtual FOnModelChanged& OnModelChanged() override { return OnModelChangedDelegate; }

	// The producer-side entry every carrier funnels a normalized hint into. Broadcasts OnModelChanged on the
	// game thread. A carrier builds the hint (DecodeContainerIdFromState for the 139, MakeHintFromPing for the
	// ping) and calls this; it never re-pulls directly.
	void NotifyModelChanged(const FCrowdyModelChangeHint& Hint);

	// Decode the changed container's id from a SERVER_EVENT (139) state payload (an ASCII string the authored
	// notification writes there). False when absent/empty. Static + public so the carrier-normalization is
	// headless-testable with a raw byte array (no live message).
	static bool DecodeContainerIdFromState(TConstArrayView<uint8> StateBytes, FString& OutContainerId);

	// Build a hint from a fallback ping (opcode 138). The ping names both the entity and the container; the hint
	// prefers the container downstream (each client may bind its own entity to a shared container).
	static FCrowdyModelChangeHint MakeHintFromPing(const FCrowdyModelChangedPing& Ping);

	// Decode the changed container's id from a CHANNEL_MESSAGE (opcode 18) payload authored by a model-driven
	// channel notification: the payload is concat(ModelChangedChannelPrefix, id). Returns false for any payload
	// that does not carry our prefix, so ordinary channel traffic (chat, reliable-RPC frames) is ignored. Accepts
	// both a raw-ASCII payload and a base64-encoded one, since either encoding may arrive. Static + public for tests.
	static bool DecodeChannelModelChangedId(const TArray<uint8>& Payload, FString& OutContainerId);

	// Decode a SIGNAL from a CHANNEL_MESSAGE (opcode 18) payload authored by an effect's signal notification: the
	// payload is concat(SignalChannelPrefix, "<name>:", id). Returns false for anything without our prefix, so
	// model-changed frames, chat and reliable-RPC frames are all ignored. Handles the same raw-ASCII and base64
	// encodings as the model-changed decoder. Static + public for tests.
	//
	// The payload is untrusted: the name is bounded and character-checked before it is ever turned into a function
	// name, so a forged frame cannot name an arbitrary UFUNCTION to call.
	static bool DecodeChannelSignal(const TArray<uint8>& Payload, FString& OutSignalName, FString& OutContainerId);

	// Decode a session-changed cue from a CHANNEL_MESSAGE (opcode 18) payload: exactly
	// concat(SessionChangedChannelPrefix, "<session id>|<revision>|<kind>"), in the same raw-ASCII and base64
	// encodings the other two channel decoders accept. Returns false for anything without the prefix, an empty id,
	// a revision that is not a non-negative integer, an empty kind or a kind over 64 characters: the payload is
	// attacker-reachable and the kind is handed straight to Blueprints. Static + public for tests.
	static bool DecodeChannelSessionCue(const TArray<uint8>& Payload, FString& OutSessionId, int64& OutRevision,
		FString& OutKind);

	// Self-echo drop (see RecentlySelfActed below). MarkSelfActed records ContainerId as just-invoked by this
	// client; ConsumeSelfEcho returns true (and consumes the record) when a model-changed echo arrives for a
	// container this client just invoked, so the redundant self re-pull is skipped. Public for headless tests.
	void MarkSelfActed(const FString& ContainerId);
	bool ConsumeSelfEcho(const FString& ContainerId);

	// Whether a participant of this class should pull its container's server state once on first bind, so its live
	// members reflect server truth immediately instead of waiting for the next model-changed notification.
	//
	// Pull on start is a setting of the container type, not of a placed instance: a class that is not a Game Model
	// container has no such setting and pulls, and a container follows its meta=(CrowdyPullOnStart) tag, which is
	// absent unless the author turned the pull off. Pure (it reads only class metadata, or the baked table in a
	// packaged build), so the gate is headless-testable without a live ResolveOrCreateContainer round trip.
	static bool ShouldPullOnBindForClass(const UClass* Class);

	// Resolves this subsystem from a completion only while its world session is still live. The Game API client is
	// owned by the game instance and outlives this world subsystem, so a completion can arrive after the world was
	// torn down: a weak pointer still resolves during that window even though every cache it would write into has
	// already been cleared. Anything that MUTATES subsystem state, or issues further server work on behalf of the
	// world, must resolve through here instead of a bare TWeakObjectPtr::Get().
	// This is only ever a gate on TOUCHING STATE, never on notifying the caller: the completion must still run and
	// must still tell its caller the outcome, so a latent Blueprint action's pins fire and it unroots itself. When
	// this returns null, stop the chain and report a failure - do not simply return.
	// The world session is started in Initialize() and ended in Deinitialize(), so an instance that never ran
	// Initialize (a NewObject in a headless test) never resolves live.
	static UCrowdyGameModelSubsystem* ResolveLive(const TWeakObjectPtr<UCrowdyGameModelSubsystem>& Weak);

	// The server's per-player, per-app gameModelInvoke allowance, shared across every Game Model call the game makes
	// and NOT just this one effect. Mirrored here so the governor can see the ceiling coming instead of only learning
	// about it from a refusal. If the server's limits change, these are the two numbers to change.
	static constexpr int32 InvokeBudgetWindowSeconds = 10;
	static constexpr int32 InvokeBudgetLimitPerWindow = 120;

	// Bounds on how far the governor may stretch an authored coalesce window, and the absolute ceiling no window may
	// pass however much of the allowance is spent. A stretched window trades latency for allowance, so the cap is what
	// stops relief from turning into an effect that visibly lands seconds late.
	static constexpr float MaxCoalesceStretch = 8.0f;
	static constexpr float MaxCoalesceWindowSeconds = 2.0f;

	// How many merge windows may be open at once.
	//
	// A window is per TARGET, so merging only ever saves calls for repeated applies to the SAME container: applies
	// spread across many targets each need their own window and their own call, and this is therefore also a bound on
	// how many distinct targets one client can be mid-effect on at once. Past it the coalescer closes its OLDEST
	// window early to make room rather than giving up on merging, because the moment it fills is the moment the
	// allowance is tightest and sending every further apply on its own spends it fastest.
	static constexpr int32 MaxOpenCoalesceWindows = 64;

	// The local backoff bounds and retry count for a refused invoke. Deliberately conservative: a retry spends
	// allowance too, so a long refusal storm must decay into giving up rather than into more calls.
	//
	// The ceiling is DERIVED from the budget window rather than picked, and it has to be at least it. The server's
	// retryAfterMs is what remains of that window, so a refusal arriving early in one names very nearly the whole
	// window; a ceiling below it would clamp the wait to less than the server asked for and retry while the window is
	// still shut, spending one of only two retries on a call that cannot succeed. The extra second is slack for the
	// round trip the number does not account for.
	static constexpr float MinBudgetRetryDelaySeconds = 1.0f;
	static constexpr float MaxBudgetRetryDelaySeconds = static_cast<float>(InvokeBudgetWindowSeconds) + 1.0f;
	static constexpr int32 MaxBudgetRetries = 2;

	// How the re-bind sweep for entities that have not resolved a container yet is rate limited. A model-changed
	// notification for a container nothing here holds is the signal that a pending entity MIGHT now be resolvable, and
	// those notifications arrive at the rate the whole session mutates state, so the sweep is driven off a timer
	// instead: however many notifications land, at most one sweep runs per interval. Within a sweep only
	// MaxPendingResolvesPerSweep entities are re-driven, because each one is a network round trip and a busy world can
	// hold hundreds; the rest are picked up by the following sweep. Each entity then waits out its own doubling
	// backoff, so an entity whose container genuinely does not exist yet stops being asked about every interval.
	static constexpr float PendingSweepIntervalSeconds = 1.0f;
	static constexpr int32 MaxPendingResolvesPerSweep = 16;
	static constexpr float MinPendingRetryDelaySeconds = 1.0f;
	static constexpr float MaxPendingRetryDelaySeconds = 30.0f;

	// How long notifications for one bound container are gathered before it is pulled. A fight touching a bound
	// row changes it many times a second and every change notifies, so pulling per notification turns one fight
	// into one round trip per change per row. Short enough that a value a player is reading still lands promptly.
	// Stretched by StretchCoalesceWindowSeconds as the allowance is spent, exactly as an invoke's window is: a
	// window that never widens emits pulls at a fixed rate into an allowance the pulls themselves are spending.
	static constexpr float RefreshPullCoalesceSeconds = 0.1f;

	// How many CrowdyContainer components one entity may be stood in for. The class an entity records is chosen by
	// its own owner and each container it names costs a binding and a retrying resolve here, so the multiplier is
	// bounded rather than left to the declaration. Well above any real actor: the row's own container plus a
	// handful of component ones.
	static constexpr int32 MaxDerivedContainersPerEntity = 8;

	// How long a coalesce window should actually stay open, given the author's window and how much of the allowance
	// the last window's worth of calls already spent. Below a relief point the authored window is returned unchanged,
	// so an effect behaves exactly as tuned whenever the allowance is not under pressure; above it the window grows
	// towards MaxCoalesceStretch, bounded by MaxCoalesceWindowSeconds. Never returns less than the authored window
	// (the governor may only merge harder, never less). Pure + static so the curve is table-testable.
	static float StretchCoalesceWindowSeconds(float AuthoredSeconds, int32 RecentInvokes, int32 Limit);

	// How long to wait before retrying a refused invoke. ServerSuggestedMs is the server's own retry-after, read off
	// the refusal's extensions.retryAfterMs and unset when it named none. A suggestion wins when present, clamped like
	// any other untrusted server value so a bad one cannot park a caller indefinitely; otherwise a doubling local
	// backoff runs. Set-but-zero takes the suggestion branch, not the fallback: it means the window has already
	// rolled, which is a different instruction from having named no wait at all.
	//
	// The local fallback is permanent rather than a placeholder. The server carries a wait only when it knows one, so
	// a refusal without it is always possible and still has to back off somehow. Pure + static.
	static float ResolveBudgetRetryDelaySeconds(int32 AttemptIndex, TOptional<int64> ServerSuggestedMs);

	// How long to wait before re-driving one pending entity's container resolve. A doubling backoff on the entity's own
	// attempt count, clamped into [Min, Max]: an entity whose owner has not created its row yet is asked about less and
	// less often instead of once per sweep forever. Pure + static so the curve is table-testable.
	static float ResolvePendingRetryDelaySeconds(int32 AttemptIndex);

	// Whether a finished invoke may be retried with the IDENTICAL request. True only for the server's own rate-limit
	// refusal, identified by its budget attribution AND its fault code together. Neither alone is enough: the
	// attribution would admit some other budget-shaped failure, and the code alone a mis-mapped or forged one.
	//
	// It requires a FAILED transport, which is the inverse of what it once asked for. This refusal is thrown rather
	// than returned in band, so it never has a clean one and requiring a clean one closed the gate entirely. Asking
	// for a failed one keeps it as narrow as the contract: the same attribution arriving in band would mean the
	// function RAN, which is the one case where repeating could apply a write twice. The attribution then does the
	// rest, and does it better than the old flag: both fields are populated only from a GraphQL error the server
	// authored, so a dropped connection or a timeout leaves them empty and cannot reach the pair.
	//
	// This narrowness is the whole safety argument. A rate-limit refusal is decided at the server's gate before the
	// function runs, so the refused call committed nothing and repeating it cannot apply a write twice. Every other
	// failure - an unattributed transport error above all - may have committed before the failure was reported, so it
	// is never retried here. That matters most for a coalesced apply, where the payload repeated is a whole window's
	// summed magnitude rather than one apply's. Pure + static.
	static bool IsBudgetRefusal(const FCrowdyInvokeResult& Result);

	// How many Game API calls this client has made inside the budget window ending now: every call, not only
	// gameModelInvoke, because the server's allowance covers all of them and a pull spends it exactly as an invoke
	// does. Counted at the one point every call resolves its client (EnsureCppClient). The governor's view of how
	// much of the allowance is spent; also the diagnostic a test or a HUD reads.
	int32 GetRecentInvokeCount() const;

	// Whether offering an apply to the coalescer could merge anything at all right now: it needs a world (a merge
	// window is a timer) and a subsystem that is not tearing down. Callers use it to skip building a merge
	// discriminator whose only use would be to key a window that cannot be opened. It answers only whether that work
	// is worth doing, never where to send the apply: EnqueueCoalescedInvoke re-checks the same conditions and
	// dispatches on its own when they do not hold, so a caller that ignores this is correct, only more expensive.
	bool IsCoalescingAvailable() const;

	// Offer an invoke to the coalescer: merge it into an open window for the same target, function, session and
	// non-accumulated parameters, or open one, or dispatch it on its own when it cannot be merged.
	//
	// The session and (for an entity-bound apply) the target container are RESOLVED HERE, at enqueue, and both the
	// merge key and the eventual dispatch use those pinned values. That is what stops a window opened under one active
	// session, or against one container binding, from flushing its whole summed magnitude into whatever happens to be
	// current when the timer fires.
	//
	// OnDone always runs exactly once, on the game thread, whichever of those happened, and every caller merged into
	// one window receives its own copy of that one invoke's outcome. A null OnDone is a caller that wants no outcome
	// (the fire-and-forget library entry), not a caller to drop.
	void EnqueueCoalescedInvoke(const FCrowdyCoalesceRequest& Request, TFunction<void(FCrowdyInvokeResult)> OnDone);

	// Transport facade. OnDone always runs on the game thread exactly once.
	//
	// One CALL is not one round trip: a refusal the server attributes to its own rate limit is retried, after a
	// bounded backoff, up to MaxBudgetRetries times. Only that one outcome is ever repeated (see IsBudgetRefusal),
	// and only one attempt is ever in flight at a time, so a caller still sees exactly one outcome and no write can
	// be applied twice.
	void Invoke(const FCrowdyInvokeRequest& Req, TFunction<void(FCrowdyInvokeResult)> OnDone);
	void PullContainerState(const FString& ContainerId, TFunction<void(bool, TSharedPtr<FJsonObject>)> OnDone);
	void ListContainers(const FString& TypeName, const FString& SessionId,
		TFunction<void(bool, TArray<TSharedPtr<FJsonObject>>)> OnDone);

	// Associates an entity's realtime NetID with its server container id. Supplied explicitly here, or resolved
	// automatically by ResolveOrCreateContainer below.
	void BindEntityContainer(const FGuid& NetID, const FString& ContainerId);
	bool TryGetContainerId(const FGuid& NetID, FString& OutContainerId) const;

	// The container TYPE a bound server container id was bound as, or false when this client has bound no entity to
	// it. The type comes from the class declaration this client resolved at bind time and never off a payload, so a
	// holder of an entity may use it to decide which of its registrations a value is allowed to feed. One entity can
	// hold several containers, so this is the only way to tell a component container's values from its actor's.
	bool TryGetContainerTypeForContainerId(const FString& ContainerId, FName& OutTypeName) const;

	// Enrolls a stand-in for every CrowdyContainer component the class this entity records declares, so an entity
	// drawn as a row holds the same set of containers its owner's actor does. Returns how many were enrolled this
	// call; already-enrolled containers are left alone, so calling it again is a no-op. StandInOuter is the object
	// the stand-ins are outered to (the row's avatar), and the anchor's own teardown unregisters them.
	//
	// Costs no Game API call by itself. Each stand-in registering does drive a bind, which does.
	//
	// bOutShouldRetry, when given, says the answer was "not yet" rather than "none": the entity has no record, its
	// class is not loaded here, or the class chain could not be read. A caller that holds the id and asks again on
	// the next class arrival is the difference between a late-classed row binding and it never binding. Zero
	// enrolled with bOutShouldRetry false is a class that genuinely declares no container components.
	int32 EnrollDerivedComponentContainers(const FGuid& AnchorNetID, UObject* StandInOuter,
		bool* bOutShouldRetry = nullptr);

	// The sub-participant ids enrolled under this anchor, for its CrowdyContainer components. Empty for an
	// entity with none. The forward direction of the anchor link; the reverse lives on the entity record.
	void GetSubParticipants(const FGuid& AnchorNetID, TArray<FGuid>& OutSubNetIDs) const;

	/**
	 * Re-lands every value already cached for this entity's containers on whoever holds the entity now.
	 *
	 * A holder that draws an entity, loses the storage and draws it again has a fresh, empty row, while the cache
	 * here still holds every value the server has sent. A pull would diff against that cache, find nothing changed
	 * and relay nothing, so the row would sit on its spawn defaults until the server happened to write again. This
	 * hands the cached values over instead, which costs no Game API call.
	 *
	 * Covers the anchor and every sub-participant enrolled under it, because a component container's values are
	 * cached under the sub id and addressed to the anchor.
	 */
	void ReplayCachedContainerStateToSubscriber(const FGuid& AnchorNetID) const;

	// The single target-resolution seam every Blueprint entry point (the read library, the effect apply) shares:
	// resolve any registered participant - an actor carrying a UCrowdyEntityComponent, or a UObject enrolled via
	// RegisterParticipant (a Host-owned subsystem) - to its realtime NetID. False when Target is null, has no
	// world, or is not a registered entity. The entity subsystem's identity map is already object-based, so an
	// actor and a subsystem resolve through one path; callers never special-case AActor.
	bool ResolveTargetNetID(const UObject* Target, FGuid& OutNetID) const;

	// Resolve NetID's server container for TypeName. The binding key is the entity's NetID digest
	// (FCrowdyModelIdentity::NetIDToContainerKey), deterministic and identical on every client for one entity.
	// (1) cache hit; else, if this client is authoritative to create (locally owns the entity, or the entity is
	// Host-owned / shared), atomically get-or-create the row via gameModelEnsureContainer (concurrent ensures with
	// the same key converge on one row - no host election, no client-side matching); else (a remote proxy of a
	// per-player entity) do a get-by-key read and bind only if the owner's row already exists, leaving it pending
	// for a notification-driven retry otherwise. OnDone runs on the game thread once; ContainerId is empty on
	// failure. The bind fills the owner cache from the ensure/read result.
	void ResolveOrCreateContainer(const FGuid& NetID, const FString& TypeName, const FString& SessionId,
		TFunction<void(bool bOk, const FString& ContainerId)> OnDone);

	// The owner user id for a NetID, filled from the ensure/read result that bound its container, so it answers for
	// remote players too, not just the local one.
	bool TryGetCachedOwnerUserId(const FGuid& NetID, int64& OutUserId) const;

	// Reads NetID's cached canonical JSON value for a server property key (the diff basis the apply path
	// maintains). False when the entity or key is not cached. Backs the UCrowdyModel BP read library.
	bool TryGetCachedValueJson(const FGuid& NetID, const FName Key, FString& OutJson) const;

	// Invoke a function against the entity's container; on success, apply the confirmed mutationsApplied to this
	// client's cache (firing OnRep with the authoritative result, not a prediction), then emit the fallback
	// model-changed ping for peers. UCrowdyEffects::Apply wraps this to run an authored effect.
	void InvokeAndApply(const FGuid& SelfNetID, const FString& FunctionName,
		const TSharedPtr<FJsonObject>& Params, const FString& SessionId,
		TFunction<void(FCrowdyInvokeResult)> OnDone);

	// The single re-pull entry every notification carrier funnels into (fallback ping AND server-native
	// SERVER_EVENT): re-pull the entity's container state and apply it, firing OnRep for changed keys.
	void HandleModelChanged(const FGuid& NetID);

	// Container-keyed variant: find the LOCAL entity bound to ContainerId (each client may bind its own entity
	// to a shared container) and re-pull it. Both notification carriers resolve to this in the 2-client case.
	void HandleModelChangedByContainer(const FString& ContainerId);

	// World-free apply seams (public so headless tests exercise diff + OnRep with a NewObject target, no
	// world, no live server). ApplyStateToContainer diffs NewState against the NetID-keyed cache and fires
	// each changed key's parameterless CrowdyOnRep on Container; ApplyMutationsToContainer does the same
	// from a confirmed invoke's mutationsApplied (key -> newValueJson).
	void ApplyStateToContainer(const FGuid& NetID, UObject* Container, const TSharedPtr<FJsonObject>& NewState);
	void ApplyMutationsToContainer(const FGuid& NetID, UObject* Container, const TArray<FCrowdyMutationApplied>& Mutations);

	// Routes a confirmed invoke's mutations to the container each one NAMES, not to the invoke's own container.
	// An effect may write source.<attr> as well as self.<attr> (crediting the attacker while damaging the target),
	// so the two are frequently different objects; applying the whole set to the invoke target wrote the source's
	// values onto a class that does not declare them, where they were silently dropped. Each destination resolves
	// to its bound participant, else to the by-id cache when it is a watched free/data container, else is skipped.
	// SelfNetID may be invalid (the by-id invoke path); SelfContainerId is the fallback destination for a mutation
	// carrying no container id, which is what a server predating the field sends. Public alongside the other
	// world-free apply seams so a headless test can route across two participants with no world and no server.
	void ApplyInvokeMutations(const FGuid& SelfNetID, const FString& SelfContainerId,
		const TArray<FCrowdyMutationApplied>& Mutations);

	// Session lifecycle. Each facade fills endpoint/token/appId from the session exactly like Invoke, then
	// marshals the call through FCrowdyGameApiCodec. OnDone always runs on the game thread once; a transport/logic failure
	// surfaces bOk=false with an empty result and a traced reason, and the server's refusal is recorded so
	// GetLastModelError / GetLastModelErrorCode can be read afterwards (SESSION_FULL, SESSION_LOCKED,
	// SESSION_HOST_TERM_STALE and the like). All use a plain app-scoped token, never an admin (manage_apps) one.
	// Every Session Id falls back to the active session when empty (ResolveSessionId).
	void CreateSession(const FString& Name, const TArray<int64>& ParticipantUserIds, const FString& MetadataJson,
		const FCrowdyGameModelCreateSessionOptions& Options,
		TFunction<void(bool bOk, const FCrowdyGameModelSession& Session)> OnDone);
	// Create with every option left to the server default.
	void CreateSession(const FString& Name, const TArray<int64>& ParticipantUserIds, const FString& MetadataJson,
		TFunction<void(bool bOk, const FCrowdyGameModelSession& Session)> OnDone);
	// Join delivers the participant row, whose Incarnation a later Leave must name; this subsystem remembers it (see
	// GetRememberedSessionIncarnation). With bBindPresenceToOwnActor the local client's own Buddy actor uuid rides
	// along, so the session's presence tracking follows that actor.
	void JoinSession(const FString& SessionId, const FString& Role, bool bBindPresenceToOwnActor,
		TFunction<void(bool bOk, const FCrowdyGameModelSessionParticipant& Participant)> OnDone);
	// Success-only form for callers that do not need the row; presence is not bound to an actor.
	void JoinSession(const FString& SessionId, const FString& Role, TFunction<void(bool bOk)> OnDone);
	// Incarnation <= 0 means the one remembered from this subsystem's own Create/Join; with none remembered the call
	// fails locally, because the server requires it.
	void LeaveSession(const FString& SessionId, int32 Incarnation,
		TFunction<void(bool bOk, const FCrowdyGameModelSessionParticipant& Participant)> OnDone);
	// Host controls. ExpectedHostTerm > 0 is sent as given; UseKnownHostTerm (0) sends the HostTerm this subsystem
	// last read for the session, if any, so a host that has been replaced is refused instead of applied;
	// SkipHostTermCheck (-1) sends none.
	static constexpr int32 UseKnownHostTerm = 0;
	static constexpr int32 SkipHostTermCheck = -1;
	void SetSessionAdmission(const FString& SessionId, ECrowdySessionAdmission Admission, int32 ExpectedHostTerm,
		TFunction<void(bool bOk, const FCrowdyGameModelSession& Session)> OnDone);
	void TransferSessionHost(const FString& SessionId, int64 ToUserId, int32 ExpectedHostTerm,
		TFunction<void(bool bOk, const FCrowdyGameModelSession& Session)> OnDone);
	// Reason Completed or Abandoned; anything else is sent as Completed.
	void EndSession(const FString& SessionId, ECrowdySessionEndReason Reason, int32 ExpectedHostTerm,
		TFunction<void(bool bOk, const FCrowdyGameModelSession& Session)> OnDone);
	// Set (bHasUserId) or clear (!bHasUserId) whose turn it is; the returned session carries the new turn holder.
	void SetSessionTurn(const FString& SessionId, int64 UserId, bool bHasUserId,
		TFunction<void(bool bOk, const FCrowdyGameModelSession& Session)> OnDone, int32 ExpectedHostTerm = UseKnownHostTerm);
	// HostUserId and Limit are skipped when 0.
	void ListSessions(ECrowdySessionStatusFilter Status, ECrowdySessionAdmissionFilter Admission, int64 HostUserId,
		int32 Limit, TFunction<void(bool bOk, const TArray<FCrowdyGameModelSession>& Sessions)> OnDone);
	// Word forms: Status ("active", "completed", "abandoned") and Admission ("open", "locked", "closed") are the
	// server's words, empty for any.
	void ListSessions(const FString& Status, const FString& Admission, int64 HostUserId, int32 Limit,
		TFunction<void(bool bOk, const TArray<FCrowdyGameModelSession>& Sessions)> OnDone);
	void ListSessions(const FString& Status, TFunction<void(bool bOk, const TArray<FCrowdyGameModelSession>& Sessions)> OnDone);
	void GetSession(const FString& SessionId, TFunction<void(bool bOk, const FCrowdyGameModelSession& Session)> OnDone);
	// The session with its participants as of one revision: the resync read after a cue or a revision gap.
	void GetSessionSnapshot(const FString& SessionId,
		TFunction<void(bool bOk, const FCrowdyGameModelSessionSnapshot& Snapshot)> OnDone);
	// The change log after AfterRevision (0 for everything), at most Limit entries (<= 0 for the server default).
	void GetSessionEvents(const FString& SessionId, int64 AfterRevision, int32 Limit,
		TFunction<void(bool bOk, const TArray<FCrowdyGameModelSessionEvent>& Events)> OnDone);

	// Maps the codec's plain session data to the Blueprint-facing structs at the facade edge. Public so a headless
	// test can check the mapping with hand-built data.
	static FCrowdyGameModelSession ToBpSession(const FCrowdyGameSessionData& Data);
	static FCrowdyGameModelSessionParticipant ToBpParticipant(const FCrowdyGameSessionParticipantData& Data);
	// Fills the typed detail fields from PayloadJson where the server sent them.
	static FCrowdyGameModelSessionEvent ToBpSessionEvent(const FCrowdyGameSessionEventData& Data);
	static FCrowdyGameModelSessionSnapshot ToBpSnapshot(const FCrowdyGameSessionSnapshotData& Data);

	// The server's words for each enum and back; an unknown word maps to the enum's Unknown / Other member.
	static ECrowdySessionStatus ParseSessionStatus(const FString& Word);
	static ECrowdySessionAdmission ParseSessionAdmission(const FString& Word);
	static const TCHAR* SessionAdmissionWord(ECrowdySessionAdmission Admission);
	static ECrowdySessionPresence ParseSessionPresence(const FString& Word);
	static const TCHAR* SessionPresenceWord(ECrowdySessionPresence Presence);
	static ECrowdySessionEndReason ParseSessionEndReason(const FString& Word);
	static ECrowdySessionParticipantState ParseParticipantState(const FString& Word);
	static ECrowdySessionLeftReason ParseLeftReason(const FString& Word);
	static ECrowdySessionEventKind ParseSessionEventKind(const FString& Word);
	static ECrowdySessionError ClassifySessionError(const FString& Code);

	// The incarnation a leave sends: an explicit one (> 0) wins, else the remembered one when there is one, else
	// false. Pure + static so the precedence is headless-testable.
	static bool ResolveLeaveIncarnation(int32 Explicit, const int32* Remembered, int32& Out);
	// The host term a host action sends: > 0 as given, UseKnownHostTerm the remembered one (0 when none),
	// SkipHostTermCheck nothing (0).
	static int32 ResolveHostTerm(int32 Requested, const int32* Known);

	// The incarnation this subsystem's own Create/Join recorded for a session, or 0 when it has none.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Sessions & Turns|Advanced", meta = (DisplayName = "Get Remembered Session Incarnation"))
	int32 GetRememberedSessionIncarnation(const FString& SessionId) const;

	// The HostTerm this subsystem last read for a session (from any call that returned it), or 0 when it has none.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Sessions & Turns|Advanced", meta = (DisplayName = "Get Known Session Host Term"))
	int32 GetKnownSessionHostTerm(const FString& SessionId) const;

	// The most recent refused session call as something a Blueprint can switch on, with the server's code and
	// message. Error is None when the last call succeeded.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Sessions & Turns", meta = (DisplayName = "Get Last Session Failure"))
	FCrowdyModelFailure GetLastFailure() const;

	// Opens a session's event stream over the WebSocket and broadcasts each event on OnSessionChanged. AfterRevision
	// < 0 starts from now; otherwise the server replays everything after it. Watching the same id again replaces the
	// earlier watch. Closed by UnwatchSession and on world teardown.
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Sessions & Turns", meta = (DisplayName = "Watch Game Session"))
	void WatchSession(const FString& SessionId, int64 AfterRevision = -1);
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Sessions & Turns", meta = (DisplayName = "Unwatch Game Session"))
	void UnwatchSession(const FString& SessionId);

	// Broadcast on the game thread for every session change heard from either carrier. A channel cue carries no
	// payload (PayloadJson empty): the game re-pulls the snapshot; a gap in Revision means pull the snapshot again.
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Sessions & Turns", meta = (DisplayName = "On Game Session Changed"))
	FCrowdyOnSessionChanged OnSessionChanged;

	// The local player's Game Model user id (from UCrowdyGameSession), or 0 when signed out.
	int64 GetLocalUserId() const;
	// True when the session's current turn holder is the local user (client-side input gating; the server's
	// is_current_turn policy is the real enforcement).
	bool IsLocalUsersTurn(const FCrowdyGameModelSession& Session) const;

	// Default session context. Set the active session once and every Game Model call that takes a Session Id may
	// leave that pin empty to operate on it, so gameplay code stops threading the same session string through every
	// node. World-scoped: cleared on world teardown. An explicit Session Id on a call still wins over the active one.
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Sessions & Turns", meta = (DisplayName = "Set Active Crowdy Session"))
	void SetActiveSession(const FString& SessionId);
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Sessions & Turns", meta = (DisplayName = "Get Active Crowdy Session"))
	FString GetActiveSession() const;
	UFUNCTION(BlueprintCallable, Category = "Crowdy SDK|Game Model|Sessions & Turns", meta = (DisplayName = "Clear Active Crowdy Session"))
	void ClearActiveSession();

	// The single point of truth for the session-resolution rule so it cannot drift across call sites: an explicit
	// (non-empty) session id wins; otherwise the active session; otherwise empty (an app-global call). Pure + static
	// so every subsystem boundary resolves identically and the order is headless-testable.
	static FString ResolveSessionId(const FString& Explicit, const FString& Active);

	// Last-error cache. The most recent server/transport error string the subsystem observed (or a caller set), so a
	// UI can read a human reason after a failed call without threading it through every callback. Game-thread only.
	void SetLastModelError(const FString& InError);
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Advanced", meta = (DisplayName = "Get Last Crowdy Model Error"))
	FString GetLastModelError() const;
	// The server's stable code for the most recent refused session call (SESSION_FULL, SESSION_LOCKED, ...), empty
	// when the failure carried none. Branch on this, not on the human-readable error string.
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Game Model|Advanced", meta = (DisplayName = "Get Last Crowdy Model Error Code"))
	FString GetLastModelErrorCode() const;

	// Free/data containers addressed by containerId, with no actor. Create/pull/invoke/set-property, backed by a
	// per-container cache the typed getters read and the OnDataContainerChanged delegate a UI binds to. A
	// pulled/created/invoked container is watched so a later model-changed notification for it re-pulls.
	void CreateDataContainer(const FString& TypeName, const FString& DisplayName, const FString& SessionId,
		const FString& MetadataJson, TFunction<void(bool bOk, const FString& ContainerId)> OnDone);
	// Pull a container's visible state by id into the by-id cache, broadcasting OnDataContainerChanged when any
	// value changed. Starts watching the container.
	void PullDataContainer(const FString& ContainerId, TFunction<void(bool bOk)> OnDone);
	// Invoke a function against a container by id (not an actor). On success, apply the confirmed mutations to the
	// by-id cache (broadcasting the change), then emit the fallback model-changed ping so peers re-pull.
	void InvokeOnContainer(const FString& ContainerId, const FString& FunctionName,
		const TSharedPtr<FJsonObject>& Params, const FString& SessionId, TFunction<void(FCrowdyInvokeResult)> OnDone);
	// Direct property write (owner/admin-writable props). ValueJson is a JSON-encoded value ("\"Aria\"", "42").
	// A direct write is not in the event log, so the local cache + delegate refresh only when the container is
	// already watched (pulled/created/invoked/WatchDataContainer); otherwise call PullDataContainer to observe it.
	void SetDataProperty(const FString& ContainerId, const FString& Key, const FString& ValueType,
		const FString& ValueJson, TFunction<void(bool bOk)> OnDone);
	// Read a watched container's cached canonical JSON value for a key (backs the UCrowdyGameModel BP getters).
	bool TryGetContainerValueJson(const FString& ContainerId, const FName Key, FString& OutJson) const;
	// Start/stop caching + notification-driven re-pull for a container without an immediate pull (a UI that binds
	// OnDataContainerChanged before the first change). Watching is also implied by pull/create/invoke.
	void WatchDataContainer(const FString& ContainerId);
	void UnwatchDataContainer(const FString& ContainerId);

	// The container graph. Edges are directed (from -> to) with a relationship label; Traverse walks up to Depth
	// hops (clamped to 5), ListChildren is the depth-1 node list (an inventory's items, a chest's loot).
	void AddEdge(const FString& FromContainerId, const FString& ToContainerId, const FString& RelationshipType,
		float Weight, bool bHasWeight, const FString& MetadataJson,
		TFunction<void(bool bOk, const FCrowdyContainerEdge& Edge)> OnDone);
	void Traverse(const FString& RootId, const FString& RelationshipType, int32 Depth,
		TFunction<void(bool bOk, const TArray<FCrowdyContainerRef>& Nodes, const TArray<FCrowdyContainerEdge>& Edges)> OnDone);
	void ListChildren(const FString& RootId, const FString& RelationshipType,
		TFunction<void(bool bOk, const TArray<FCrowdyContainerRef>& Children)> OnDone);

	// Model Collections: a container that owns other containers via typed edges (an inventory of items, a party of
	// members). A collection is the set of RelationshipType edges from a parent container to item containers; the
	// SDK provisions a reserved crowdy_rev counter + a per-type touch function (see the schema sync) so an edge
	// change - which is a graph mutation, not a property write - still produces a model-changed notification a
	// watcher can react to. AddToCollection links an item and best-effort invokes the parent's touch function so
	// peers watching the parent re-pull and re-read the collection; RemoveFromCollection resolves the edge (a
	// depth-1 traverse, since delete is by edge id) then deletes it and touches. Both need ParentTypeName (the
	// parent's CrowdyContainer type) to name its touch function; a blank type skips the touch (the edge still
	// changes, but peers refresh only on their next pull). bOk reflects the edge operation; the touch is best-
	// effort. ListChildren above IS "Get Collection" (the depth-1 item list). OnDone runs on the game thread once.
	void AddToCollection(const FString& ParentContainerId, const FString& ParentTypeName,
		const FString& ItemContainerId, const FString& RelationshipType, TFunction<void(bool bOk)> OnDone);
	void RemoveFromCollection(const FString& ParentContainerId, const FString& ParentTypeName,
		const FString& ItemContainerId, const FString& RelationshipType, TFunction<void(bool bOk)> OnDone);

	// "Get Collection With Items' State": list the parent's items AND fetch each item's visible state in one call,
	// so a UI paints a whole bag without a per-item pull. Bounded to MaxItems (<=0 defaults to 64) to cap the
	// per-item state fan-out; a larger collection is truncated with a warning. Pure read: it does NOT watch or
	// cache the items or broadcast a change (a get is not a change), so item state is returned as JSON on each
	// FCrowdyCollectionItem. OnDone runs on the game thread once with the (possibly-truncated) item set.
	void GetCollectionWithState(const FString& ParentContainerId, const FString& RelationshipType, int32 MaxItems,
		TFunction<void(bool bOk, const TArray<FCrowdyCollectionItem>& Items)> OnDone);

	// Delete instances by id (server-enforced owner-or-admin). DeleteContainer drops all local state for
	// the id on success (the free/data by-id cache + watch set, and for an actor-bound container every per-NetID
	// trace - binding, attribute cache, owner cache - mirroring HandleEntityUnregistered) and broadcasts OnDataContainerChanged
	// (the removal is the change); the server cascades the container's properties + every connected edge. DeleteEdge
	// removes one directed graph edge (source-container owner or admin); edges are not cached, so nothing local
	// changes. bSuccess is a clean transport -- an authorization/refusal failure is a false.
	void DeleteContainer(const FString& ContainerId, TFunction<void(bool bSuccess)> OnDone);
	void DeleteEdge(const FString& EdgeId, TFunction<void(bool bSuccess)> OnDone);

	// World-free apply seam for a free/data container (public so headless tests exercise the diff + delegate with
	// no world and no HTTP): diffs NewState against the containerId-keyed cache and broadcasts
	// OnDataContainerChanged exactly once when any value changed. Mirrors ApplyStateToContainer without the
	// live-member write (a free container has no actor to write onto). NewState is the container's COMPLETE
	// visible property set, so a cached key it omits is treated as removed.
	void ApplyDataContainerState(const FString& ContainerId, const TSharedPtr<FJsonObject>& NewState);

	// Merge seam for a free/data container: applies a confirmed invoke's changed-subset mutations onto the by-id
	// cache, touching ONLY the listed keys (never treating an absent key as removed). Use this for an invoke echo,
	// where mutationsApplied is the delta, not the full state; ApplyDataContainerState (full replace) is for a pull.
	// Broadcasts OnDataContainerChanged + the per-attribute delegate for the keys that actually changed.
	void ApplyDataContainerMutations(const FString& ContainerId, const TArray<FCrowdyMutationApplied>& Mutations);

	// Broadcast on the game thread when a watched free/data container's cached state changed after a pull.
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced", meta = (DisplayName = "On Game Model Changed (by Id)"))
	FCrowdyDataContainerChanged OnDataContainerChanged;

	// Broadcast on the game thread for every attribute whose value changed on any apply path, actor-bound or
	// free/data. The zero-setup change observer behind the "Listen for Model Changes" node: bind once instead of
	// wiring a per-attribute OnRep, then filter by Target or ModelId if desired. Fires alongside (not instead of)
	// each changed attribute's CrowdyOnRep. See FCrowdyModelAttributeChanged.
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced", meta = (DisplayName = "On Model Attribute Changed"))
	FCrowdyModelAttributeChanged OnModelAttributeChanged;

	// Broadcast on the game thread when a signal arrives, after the bound container's OnSignal_<Name> handler has
	// run. The handler is the usual way to react; this is for anything that is not the container itself, such as UI
	// or an audio system, and it still fires when the named container is not bound locally (Target is null then).
	// Target is whatever ran the handler, else the object bound to the container.
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Game Model|Advanced", meta = (DisplayName = "On Crowdy Signal"))
	FCrowdySignalReceived OnCrowdySignal;

	// Route one decoded signal: call OnSignal_<Name> on the locally bound container when there is one, then
	// broadcast OnCrowdySignal. A container held by a stand-in is retried on the entity holding it, nothing else is.
	// Public so a headless test can drive dispatch with no channel and no network.
	void DispatchSignal(const FString& SignalName, const FString& ContainerId);

	// Whether this subsystem's world is the one its game instance is currently in. The service registry is
	// game-instance scoped while this subsystem is per-world, so a world left behind by a level travel keeps its
	// subscription until it is collected and would otherwise handle every delivery a second time. False for such
	// a departed world, true for exactly one live world. Public so a test can drive it without a travel.
	bool IsCurrentWorldForGameInstance() const;

#if !UE_BUILD_SHIPPING
	// Opens the server's container-change feed over a WebSocket and logs every notification it delivers. This is a
	// diagnostic rather than a carrier: nothing is re-pulled from it and no cache is written, so it proves the
	// subscription path reaches a live server without changing how a change actually reaches a client today.
	// TypeName narrows the feed to one container type, or watches every type when empty. Driven by
	// crowdy.gamemodel.watchcontainers.
	void DebugWatchContainerChanges(const FString& TypeName);

	// Closes the feed above. Idempotent, and run on teardown so a PIE session always exits with the socket shut.
	void DebugStopWatchingContainerChanges();
#endif

#if WITH_DEV_AUTOMATION_TESTS
	// Injects the entity subsystem that Initialize() normally caches, so a headless test (which never runs
	// Initialize) can exercise the real ResolveTargetNetID / ResolveEntityParticipant against enrolled participants.
	void SetEntitySubsystemForTest(UCrowdyEntitySubsystem* InEntities) { EntitySubsystemForEvents = InEntities; }
	// Injects the router whose entity-subscriber slot the relay reads, for the same reason: a headless test has no
	// world to resolve one from. The subscriber is registered on the router itself, exactly as it is at runtime,
	// so a test still exercises the one registration rather than a second one that exists only for tests.
	void SetEventRouterForTest(UCrowdyEventRouter* InRouter);
	UCrowdyEventRouter* GetEventRouterForTest() const;
	// Drives the private auto-bind handlers so a headless test exercises the real component sweep (enrollment,
	// filtering, idempotency, anchor->sub tracking) and the teardown cascade, which Initialize normally wires to the
	// entity subsystem's delegates. The container bind these queue is a no-op without an API context (no world/session).
	void HandleEntityRegisteredForTest(const FGuid& NetID) { HandleEntityRegistered(NetID); }
	void HandleEntityUnregisteredForTest(const FGuid& NetID) { HandleEntityUnregistered(NetID); }
	// Subscribes to the entity subsystem's lifecycle delegates exactly as Initialize does, so a headless test
	// drives registration and teardown through the real chain instead of calling the handlers at the points it
	// expects them. A test that calls the handlers by hand cannot see a release that should have fired one.
	void BindEntityLifecycleForTest(UCrowdyEntitySubsystem* Entities);
	// Automation only. Starts the world session Initialize() normally owns, so a headless test (which never runs
	// Initialize) can drive the paths that are gated on a live session.
	void BeginWorldSessionForTest()
	{
		WorldSessionToken = MakeShared<uint8>(0);
	}
	// Takes the model-changed subscription against a registry a test owns, using the same call the reception
	// registration makes, so a test drives the production delivery path rather than a copy of it.
	void SubscribeToModelChangedCarriersForTest(FCrowdyServiceRegistry& Registry)
	{
		SubscribeToModelChangedCarriers(Registry);
	}
	// Exercises the private participant -> class resolution ShouldPullOnBind uses, so a headless test proves the
	// real participant wiring and not just the class-level decision it defers to.
	static bool ShouldPullOnBindForTest(const UObject* Participant) { return ShouldPullOnBind(Participant); }
	// Exercises the retry completion's own gate, which treats "the participant no longer resolves" as a reason to
	// skip the pull rather than as just another no-entity-component case.
	static bool ShouldPullOnRetryForTest(const UObject* Participant) { return ShouldPullOnRetry(Participant); }
	// Automation only, and destructive: it ends the world session exactly as Deinitialize() does, WITHOUT clearing
	// the caches Deinitialize also clears. On a subsystem that is still in use this turns it into a silent write
	// sink - completions keep telling their callers plausible outcomes while nothing lands in any cache and no
	// OnRep fires, with no error anywhere. Only ever call it on a subsystem the test itself created, which is why
	// it refuses to run on an instance that belongs to a world.
	void ForceEndWorldSessionForTest()
	{
		if (!ensureMsgf(GetWorld() == nullptr,
			TEXT("[GameModel] ForceEndWorldSessionForTest was called on a subsystem that belongs to a live world; refusing.")))
		{
			return;
		}
		WorldSessionToken.Reset();
	}
	// Writes one attempt into the sliding-window ledger at a time the test picks, so the governor's pressure curve can
	// be driven to any point without making real calls or waiting out a real window.
	void RecordInvokeAttemptForTest(double Now) { RecordInvokeAttempt(Now); }
	// How many merge windows are open right now, so a test can prove that two applies merged into one and that a third
	// with a different discriminator did not.
	int32 GetOpenCoalesceWindowCountForTest() const { return PendingCoalesced.Num(); }
	// Binds an entity to a container and then breaks that binding the way a despawn does, so a test can drive the
	// "the window's target went away while it was open" path without an entity subsystem or a live registration.
	void BindEntityContainerForTest(const FGuid& NetID, const FString& ContainerId) { BindEntityContainer(NetID, ContainerId); }
	// Records the container TYPE a bind resolved, which BindParticipantContainer normally writes from the
	// declaring class. Lets a test bind two containers of different types to one entity without standing up the
	// classes and the class registry that would name them.
	void RecordContainerTypeForTest(const FGuid& NetID, const FString& TypeName) { ContainerTypeByNetID.Add(NetID, TypeName); }
	void UnbindEntityContainerForTest(const FGuid& NetID) { HandleEntityUnregistered(NetID); }
	// How many local entities are bound to one container row, so a test can prove the multi-bind shape it set up
	// exists (and that unbinding removed one) before asserting which of them a notification reaches.
	int32 GetBoundNetIDCountForTest(const FString& ContainerId) const
	{
		const TArray<FGuid>* Bound = ContainerIdToNetIDs.Find(ContainerId);
		return Bound ? Bound->Num() : 0;
	}
	// The epoch stamped on a binding right now, so a test can snapshot it the way a dispatched invoke does.
	uint32 GetBindEpochForTest(const FGuid& NetID) const
	{
		const uint32* Found = BindEpochByNetID.Find(NetID);
		return Found ? *Found : 0;
	}
	// The rebind question a retry asks when its timer fires, answered against a snapshot taken earlier, so the guard
	// is provable with no world, no timer and no server.
	bool IsInvokeBindingStillValidForTest(const FGuid& NetID, uint32 SnapshotEpoch) const
	{
		FCrowdyInvokeBindingGuard Guard;
		Guard.bEntityBound = true;
		Guard.SelfNetID = NetID;
		Guard.BindEpoch = SnapshotEpoch;
		return IsInvokeBindingStillValid(Guard);
	}
	// The same question for a container-addressed invoke, which names its target directly and has no binding to move.
	bool IsUnboundInvokeBindingStillValidForTest() const
	{
		return IsInvokeBindingStillValid(FCrowdyInvokeBindingGuard());
	}
	// Enrolls a pending (unresolved) model entity directly, so the sweep's rate limiting and per-entity backoff are
	// drivable without a live entity subsystem, an attribute-tagged class or a server round trip.
	void AddPendingModelEntityForTest(const FGuid& NetID, const FString& TypeName)
	{
		PendingModelEntities.Add(NetID, TypeName);
	}
	// The container type an entity's registration resolved, before any server round trip. This is what separates
	// "the bind was refused because nothing declared a container" from "it resolved one and is waiting on the API".
	bool TryGetPendingModelEntityTypeForTest(const FGuid& NetID, FString& OutTypeName) const
	{
		const FString* Found = PendingModelEntities.Find(NetID);
		if (!Found)
		{
			return false;
		}
		OutTypeName = *Found;
		return true;
	}
	// The real create-versus-read gate, so a test proves a class-derived binding can never ensure a row rather
	// than just proving it was flagged as one.
	bool IsAuthoritativeToCreateForTest(const FGuid& NetID) const { return IsAuthoritativeToCreate(NetID); }
	// Whether this entity's binding was derived from the class it records rather than from its participant.
	bool IsClassDerivedBindingForTest(const FGuid& NetID) const { return ClassDerivedBindings.Contains(NetID); }
	// How many stand-ins are held strongly, so a test can force a collection and prove they survived it.
	int32 GetHeldContainerStandInCountForTest() const { return ContainerStandIns.Num(); }
	// How many times component-container enrolment was asked for, so a test can prove which caller asks.
	int32 GetDerivedEnrollmentRequestCountForTest() const { return DerivedEnrollmentRequestCount; }
	// The coalesce window a refresh pull would actually wait, which widens as the call allowance is spent.
	float GetRefreshPullWindowSecondsForTest() const { return ResolveRefreshPullWindowSeconds(); }
	// The pull decision a pending entity's retry makes, which has to answer from the recorded container class for a
	// class-derived binding rather than from the stand-in that represents it.
	bool ShouldPullOnRetryForEntityForTest(const FGuid& NetID) const { return ShouldPullOnRetryForEntity(NetID); }
	// How many sweeps have actually run, so a test can prove that N notifications produce one sweep rather than N.
	int32 GetPendingSweepCountForTest() const { return PendingSweepCount; }
	// How many container resolves started, so a test can prove a redraw that reuses a binding starts none. This is
	// the point a bind begins to spend the Game API allowance, so it is what "costs no call" has to be measured at.
	int32 GetContainerResolveStartCountForTest() const { return ContainerResolveStartCount; }
	// How many containers are waiting to be pulled, and which entity each will be pulled for, so a test can prove
	// K notifications coalesce into one pull and that the pull carries the LATEST of them.
	int32 GetPendingRefreshPullCountForTest() const { return PendingRefreshPulls.Num(); }
	bool TryGetPendingRefreshPullTargetForTest(const FString& ContainerId, FGuid& OutNetID) const
	{
		const FGuid* Found = PendingRefreshPulls.Find(ContainerId);
		if (!Found)
		{
			return false;
		}
		OutNetID = *Found;
		return true;
	}
	// Runs the drain the timer would have run, which a headless test has no timer manager to fire.
	void DrainRefreshPullsForTest() { DrainRefreshPulls(); }
	// The id a subscriber is addressed by for values pulled against this record, so a test can prove a component
	// container's values are handed to whoever holds the entity rather than to an id nobody has ever seen.
	FGuid ResolveSubscriberEntityIDForTest(const FGuid& NetID) const { return ResolveSubscriberEntityID(NetID); }
	// How many pending entities have had at least one resolve attempted, so a test can prove one sweep re-drives a
	// bounded number of them rather than every one at once.
	int32 GetPendingRetryAttemptedCountForTest() const { return PendingBindBackoff.Num(); }
	// One entity's recorded attempt count, so a test can prove the backoff stops it being re-driven every sweep.
	int32 GetPendingRetryAttemptsForTest(const FGuid& NetID) const
	{
		const FCrowdyPendingBindBackoff* Found = PendingBindBackoff.Find(NetID);
		return Found ? Found->Attempts : 0;
	}
	// Drives the two halves of the sweep separately: the rate-limited request a notification makes, and the sweep
	// itself, so a test can tell "the notification was coalesced away" from "the sweep did nothing".
	void RequestPendingModelEntitySweepForTest() { RequestPendingModelEntitySweep(); }
	void RetryPendingModelEntitiesForTest() { RetryPendingModelEntities(); }
	// Drives the teardown drain on its own, without tearing a world down, so the "every waiting caller is still told
	// what happened" property is provable in a headless test. It leaves the subsystem shut for further merging exactly
	// as Deinitialize does, so nothing after it can queue a window that would never close.
	void FailPendingWorkForTest()
	{
		bShuttingDown = true;
		FailPendingCoalesceWindows();
		FailPendingInvokeRetries();
	}
#endif

private:
	// Resolves the Game API endpoint + bearer token + app id from settings + the game session. False (with a
	// clear LogCrowdyGameModel error) when any is missing. Never logs the token.
	bool ResolveApiContext(FString& OutEndpoint, FString& OutToken, int64& OutAppId) const;

	// The async client every Game Model API call runs on. It is owned by the game instance, not by this world
	// subsystem, so it survives level travel and one pump serves every caller. Returns null when there is no game
	// instance or the client could not be constructed; a caller that gets null must still complete its own callback
	// with a failure result, or a latent node's pins never fire and any in-flight guard it took is never cleared.
	//
	// Resolving the client here is also where a call is counted against the per-player allowance, because every Game
	// Model call in this file passes through this one function on its way out. The allowance is shared by all of them
	// - ensure-container, read-by-key, list, pull, the session ops, set-property and invoke - so counting only one
	// kind would leave the governor blind to most of what spends it, and counting at the individual call sites would
	// silently stop covering whichever call is added next.
	FCrowdyCppClient* EnsureCppClient(const FString& Endpoint, const FString& Token);

	// The same client, NOT counted against the allowance. Only for work that does not consume it: the diagnostic
	// change feed opens and closes a WebSocket subscription rather than making a Game Model call.
	FCrowdyCppClient* EnsureCppClientUnmetered(const FString& Endpoint, const FString& Token);

	// The short spelling this subsystem's own completions use for ResolveLive, whose contract it shares exactly.
	static UCrowdyGameModelSubsystem* ResolveLiveSelf(const TWeakObjectPtr<UCrowdyGameModelSubsystem>& Weak)
	{
		return ResolveLive(Weak);
	}

	UCrowdyGameSession* GetGameSession() const;

	// Enroll every tagged GameInstance/World subsystem in this world as a Host-owned participant so it auto-binds
	// its container with zero per-subsystem code (the "match state subsystem" surface). A subsystem already
	// enrolled (e.g. by Subsystem Replication) is not re-registered but is still ensured bound. Idempotent per
	// world; safe to call again after level travel. Runs from OnWorldBeginPlay.
	void AutoRegisterTaggedSubsystems();

	AActor* ResolveEntityActor(const FGuid& NetID) const;
	// The object bound to NetID whether it is an actor or a non-actor participant (a Host-owned subsystem). The
	// apply + resolve seams write onto / verify the liveness of "the entity", which need not be an actor; only the
	// spatial fallback ping still needs a concrete actor. Returns null when nothing is bound.
	UObject* ResolveEntityParticipant(const FGuid& NetID) const;

	// The LOCAL entity bound to ContainerId, the reverse of TryGetContainerId. Each client binds its own entity to
	// a shared container, so this answers only for what this client holds. False when no local entity is bound.
	// Answers from the same index every other by-id path resolves through.
	bool TryGetNetIDForContainer(const FString& ContainerId, FGuid& OutNetID) const;

	// The world's entity subsystem: the cached EntitySubsystemForEvents when set (post-Initialize), else resolved
	// live from the world. One accessor so every resolver agrees on liveness and the test seam is honored.
	UCrowdyEntitySubsystem* ResolveEntitySubsystem() const;

	// Whether this client is authoritative to CREATE NetID's container (ensure it) versus only read-bind an existing
	// one: true when it locally owns the entity (a per-player container) or the entity is Host-owned (a world/shared
	// container). A shared entity is authoritative on every client because gameModelEnsureContainer is atomic - the
	// concurrent ensures converge on one row, so no host election is needed. A remote proxy of a per-player entity is
	// NOT authoritative (it must not create the owner's row) and reads by key instead.
	bool IsAuthoritativeToCreate(const FGuid& NetID) const;

	// Whether NetID's entity is Host-owned (a world/shared container such as a chest or a tagged subsystem, as
	// opposed to a per-player entity). False when the entity is unregistered.
	bool IsHostOwnedEntity(const FGuid& NetID) const;

	// Auto-bind: on entity registration, if the class carries CrowdyModel attributes + a CrowdyContainer tag,
	// resolve-or-create its container (create only when locally owned) and pull once. Unregistration drops the
	// binding + cache. Both are UFUNCTIONs so they can bind to the entity subsystem's dynamic delegates.
	UFUNCTION()
	void HandleEntityRegistered(const FGuid& NetID);
	UFUNCTION()
	void HandleEntityUnregistered(const FGuid& NetID);

	// Resolve-or-create the participant's OWN container if its class is a CrowdyContainer with model attributes
	// (the actor-as-container / subsystem path). Split out of HandleEntityRegistered so the component sweep can
	// run first and bind an actor that is not itself a container. Idempotent: a no-op when already bound.
	//
	// A participant that declares no container of its own falls back to the container declared by the class the
	// ENTITY records, which is how an entity represented here by a stand-in rather than by its own actor still
	// binds. That binding is read-only (see ClassDerivedBindings).
	void BindParticipantContainer(const FGuid& NetID, UObject* Participant);

	// Resolves whether Participant's initial bind should pull, by asking its own class through
	// ShouldPullOnBindForClass. Every participant shape answers the same way, whether it is an actor, a container
	// component enrolled as a sub-participant, or a Host-owned subsystem: the setting belongs to the container type.
	static bool ShouldPullOnBind(const UObject* Participant);

	// The retry completion's own gate: a null Participant means the entity was resolved AFTER the round trip and
	// no longer exists (it was unregistered while the resolve/create was in flight), which must skip the pull. That
	// is different from ShouldPullOnBind's own null handling, where a null/non-actor participant with no entity
	// component to consult defaults to pulling - that case is for a participant that DOES exist but has no
	// author-facing flag, not one that is gone. A non-null Participant defers to ShouldPullOnBind unchanged.
	static bool ShouldPullOnRetry(const UObject* Participant);

	// The same question for a pending entity being re-driven, asked by NetID so a binding derived from the class
	// the entity records answers from THAT class. Its stand-in participant declares nothing, so asking the object
	// would silently give every such entity the default (pull) whatever its container type says.
	bool ShouldPullOnRetryForEntity(const FGuid& NetID) const;

	// Says once, on the first entity it happens to, that an entity records a class this client never loaded. That
	// entity's container declaration is unreadable here, so it binds nothing and every effect aimed at it is
	// refused, which is otherwise indistinguishable from a server that has not created the rows yet.
	void WarnIfRecordedClassUnresolved(const FGuid& NetID, const UObject* Participant);

	// Sweep an anchor actor's components; enroll every CrowdyContainer-tagged component with model attributes as a
	// sub-participant (a reusable attributes component). Enrollment broadcasts OnEntityRegistered, so each component
	// binds its container through this same handler - no separate bind here. Records anchor -> sub ids so the
	// anchor's teardown drops them. Idempotent: an already-enrolled component is skipped.
	void RegisterComponentSubParticipants(AActor* Anchor, const FGuid& AnchorNetID);

	// Enroll one CrowdyContainer component as a sub-participant of AnchorNetID (the per-component body shared by the
	// sweep and the runtime EnrollModelComponent hook): gate on the container tag + attributes, read an optional
	// ICrowdyBindingKeyProvider key, warn on a runtime-added component with no key, register + track. Idempotent.
	void EnrollComponentSubParticipant(UActorComponent* Component, const FGuid& AnchorNetID);

public:
	// Runtime enrollment hook for a CrowdyContainer component ADDED AFTER its actor registered (the sweep only runs
	// at actor registration). Resolves the component's owner to its anchor NetID and enrolls the component, which
	// binds its container through the standard path. Call after attaching + keying a component at runtime. A no-op
	// when the component is null, its owner is not a registered entity, or it is not a bindable container.
	void EnrollModelComponent(UActorComponent* Component);

	// The symmetric teardown for EnrollModelComponent: unenroll a CrowdyContainer component being removed at runtime
	// while its actor lives on, dropping its sub-participant record + container binding + anchor tracking so nothing
	// leaks. Call before/at DestroyComponent. A no-op when the component was never enrolled. A component still
	// present when its actor tears down is handled by the anchor cascade and needs no explicit call.
	void UnenrollModelComponent(UActorComponent* Component);

private:

	// Liveness kick for pending resolves: bound to the game session's OnHostIDUpdated, which fires around
	// connect / host election - close to when the game token is first minted. It re-drives entities that could not
	// ensure at world start (no token yet) and any left pending across a host migration. It is NOT a create gate
	// (ensure is atomic; every client resolves independently); it only re-attempts pending binds.
	UFUNCTION()
	void HandleHostChanged(const FGuid& NewHostID, const FGuid& PreviousHostID);

	// Ask for a pending-entity sweep soon. This is what a notification calls: it arms the sweep timer if it is not
	// already armed, so a burst of notifications costs one sweep rather than one per notification. Sweeping directly
	// per notification is what turned an ordinary state-change storm into one network round trip per pending entity
	// per notification.
	void RequestPendingModelEntitySweep();

	// Re-attempts resolve for every entity that registered with model attributes but has not bound yet: a remote
	// proxy whose owner had not created the container when it registered, or an authoritative entity that could not
	// ensure yet (not authenticated at world start). Cheap self-heal driven by a model-changed notification for a
	// container we do not yet hold locally, or the host-change liveness kick above. Bounded to
	// MaxPendingResolvesPerSweep entities per run, each subject to its own backoff; the sweep re-arms itself while
	// anything is still pending.
	void RetryPendingModelEntities();

	// Emit the fallback FCrowdyModelChangedPing spatially (gated by crowdy.gamemodel.emitfallbackping so a
	// 2-client PIE test can rely solely on the server-native notification to isolate that path).
	void EmitModelChangedPing(const FGuid& NetID, const FString& ContainerId) const;

	// Best-effort: invoke a collection parent's reserved per-type touch function (bump crowdy_rev + emit the
	// model-changed notification) so watchers re-pull after a collection edge change. The touch runs against the
	// parent, so the server-injected $self_container_id names it in the notification (peers watching the parent re-pull
	// it). A blank ParentTypeName is a no-op (the caller could not name the touch function); an invoke failure is
	// logged, not surfaced (the authoritative edge change already succeeded).
	void TouchCollectionParent(const FString& ParentContainerId, const FString& ParentTypeName);

	// The active (default) session id every SessionId-taking call falls back to when its own pin is empty. Resolved
	// through ResolveSessionId at each subsystem boundary. World-scoped: cleared in Deinitialize.
	FString ActiveSessionId;

	// The most recent Game Model error the subsystem saw (or a caller set via SetLastModelError). Read via
	// GetLastModelError. Cleared in Deinitialize. Game-thread only.
	FString LastModelError;

	// The server's stable code for the most recent refused session call, alongside the string above.
	FString LastModelErrorCode;

	// Records a failed raw result into the two last-error strings; a clean transport records nothing.
	void RecordServerRefusal(const FCrowdyCppJsonResult& Result);

	// The shared tail of every facade whose result is one session (create, turn, read, the host controls) or one
	// participant row (join, leave): resolve the context and client, send the variables BuildVars makes for the app
	// id, record a refusal, parse FieldName out of the reply and complete once with the mapped struct.
	void RunSessionReturningOp(const TCHAR* OperationName, const TCHAR* FieldName,
		TFunction<TSharedPtr<FJsonObject>(int64 AppId)> BuildVars,
		TFunction<void(bool bOk, const FCrowdyGameModelSession& Session)> OnDone);
	void RunParticipantReturningOp(const TCHAR* OperationName, const TCHAR* FieldName,
		TFunction<TSharedPtr<FJsonObject>(int64 AppId)> BuildVars,
		TFunction<void(bool bOk, const FCrowdyGameModelSessionParticipant& Participant)> OnDone);

	// Session id -> the incarnation this subsystem's own Create (1) or Join (the row's) established, which a Leave
	// must name. Cleared in Deinitialize.
	TMap<FString, int32> SessionIncarnations;

	// Session id -> the HostTerm last read for it (every parsed session row writes here), sent with host actions so
	// a replaced host is refused. Cleared in Deinitialize.
	TMap<FString, int32> SessionHostTerms;

	// Records a session's HostTerm and, for the ordinary success path, hands it on.
	void RememberSession(const FCrowdyGameModelSession& Session);

	// Session id -> the open event-stream subscription id (WatchSession). Held as bare ids so no bridge type
	// reaches this header; all closed in Deinitialize.
	TMap<FString, uint64> WatchedSessionIds;

	// NetID -> server container id.
	TMap<FGuid, FString> NetIDToContainerId;

	// The reverse of the map above: server container id -> every local NetID bound to it. Four hot paths ask "which
	// local entity is this container?" - an inbound model-changed notification, an inbound signal, a confirmed
	// invoke's cross-container writes, and the delete cascade - and each used to answer by comparing container id
	// strings against every binding in turn. That is a linear string scan on the delivery path of every notification
	// the session produces, over a map that holds one entry per bound entity. Normally one NetID per container, but
	// the list form is required: an id is bound per-entity and nothing stops two participants sharing one row, and
	// the delete cascade has to clear all of them.
	// Kept in bind order, oldest first, which is the order FindNetIDForContainer reads it in.
	TMap<FString, TArray<FGuid>> ContainerIdToNetIDs;

	// NetID -> the container TYPE its binding was resolved as. Recorded where the binding decision is made, because
	// that is the only place the declaring class is in hand; a holder receiving values later has the container id
	// and nothing else, and a component container's values must not be read as its actor's.
	TMap<FGuid, FString> ContainerTypeByNetID;

	// NetID -> the epoch its CURRENT binding was made in. Epochs come from a counter that only ever increases, so a
	// value identifies one binding of one entity for the life of the process: an entity that unbinds and binds again
	// (a respawn, a re-key) gets a fresh one and never inherits the old one. A coalesce window records the epoch it
	// opened against, which is what stops a window opened before a despawn from flushing its summed magnitude onto
	// whatever now holds the same container row. Erased on unbind, so a stale epoch can never match again.
	TMap<FGuid, uint32> BindEpochByNetID;
	uint32 NextBindEpoch = 1;

	// NetID -> (server property key -> last-seen raw JSON value). The diff basis for OnRep-on-change.
	TMap<FGuid, TMap<FName, FString>> ContainerCache;

	// NetID -> ownerUserId, learned from listed containers' metadataJson NetID digest (local + remote owners).
	TMap<FGuid, int64> NetIDToOwnerUserId;
	// NetIDs with an in-flight ResolveOrCreateContainer, so a re-entrant registration/notification does not
	// double-list or double-create.
	TSet<FGuid> ResolveInFlight;
	// NetID -> container TypeName for model entities that registered but have not bound yet (see
	// RetryPendingModelEntities). Removed on a successful bind or on unregistration.
	TMap<FGuid, FString> PendingModelEntities;

	/**
	 * Report a server refusal that blames the app's game model, ONCE per (Code, Subject) rather than once per
	 * occurrence. These fire on gameplay paths: a bind is attempted for every entity, and an invoke can run every
	 * frame, so an unconditional warning is indistinguishable from noise and gets ignored, which is how a broken
	 * model stayed invisible behind a generic "no container bound" line for most of a day.
	 *
	 * Detail is the server's own sentence, which names what to fix. Held for the life of the session and cleared
	 * with it, so a reconnect to a repaired app reports afresh.
	 */
	void ReportModelRefusalOnce(const FString& Code, const FString& Subject, const FString& Detail);

	// The (Code, Subject) pairs ReportModelRefusalOnce has already spoken about, joined so one set covers both.
	TSet<FString> ReportedModelRefusals;

	// Entities whose container type came from the class the ENTITY records rather than from the object representing
	// it here. Such a binding is READ-ONLY for the life of the entity: the recorded class is chosen by the entity's
	// own owner, so it may say which row to read and must never authorize creating one (IsAuthoritativeToCreate
	// consults this). Held rather than recomputed because every retry asks the question again, and a role that
	// changes underneath (a host migration) must not turn a read into an ensure. Cleared on unregistration.
	TSet<FGuid> ClassDerivedBindings;

#if WITH_DEV_AUTOMATION_TESTS
	// Stands in for the router a headless test has no world to resolve. Never set outside a test.
	TWeakObjectPtr<UCrowdyEventRouter> EventRouterForTest;
#endif

	// Whether the unloaded-recorded-class warning has been said. One world's worth of crowd entities all hit the
	// same missing class, so this is said once and not once per entity.
	bool bWarnedRecordedClassUnresolved = false;

	// Whether the template-read binding key warning has been said. One world's crowd is one class, so it is said
	// once rather than once per row.
	bool bWarnedStandInBindingKeyFromTemplate = false;

	// Whether the derived-container cap has been reported. Every row of one class hits the same cap.
	bool bWarnedDerivedContainerCap = false;

	/**
	 * The registered subscriber when it holds this entity, else null. Asked once per apply rather than per key.
	 *
	 * Whether the subscriber HOLDS the id is the only question asked here. It is deliberately not "does it name
	 * a class for the id": an entity may be held with nothing declared for it, and reading the class instead
	 * would drop every server-owned value for such an entity while looking like a routing decision.
	 */
	ICrowdyEntitySubscriber* FindEntitySubscriberForEntity(const FGuid& NetID) const;

	// The id a subscriber is addressed by for values pulled against NetID: the anchor when NetID is a
	// sub-participant, otherwise NetID itself. A sub-participant's id is derived by hashing its anchor's, so a
	// holder of the entity has never seen it and answers "unknown" for it - which the router reads as a final
	// refusal. Values for a component container would then be dropped with nothing said.
	FGuid ResolveSubscriberEntityID(const FGuid& NetID) const;

	/**
	 * Hands an entity's settled values to the subscriber holding its storage.
	 *
	 * The subscriber is resolved here rather than passed in, because an apply fires notifies and delegates on
	 * its way to this call and any of them can reach the registration: a pointer taken before them could name a
	 * subscriber that has since given up the slot.
	 *
	 * Called from an apply that is already running on a pull (or on a confirmed invoke's result) the caller
	 * made, and it must stay that way: nothing on this path may start a pull of its own, per entity or
	 * otherwise, or one notification for a crowd becomes one request per entity.
	 */
	void RelayChangesToEntitySubscriber(const FGuid& NetID, const FString& ContainerId,
		TConstArrayView<FCrowdyAttributeChange> Changes) const;

	// How often one pending entity has been re-driven, and the earliest time it may be re-driven again. Kept beside
	// PendingModelEntities rather than inside it so nothing that already removes a pending entry has to learn about
	// backoff; a sweep drops any record whose entity is no longer pending, so the two cannot drift apart.
	struct FCrowdyPendingBindBackoff
	{
		double NextAttemptTime = 0.0;
		int32 Attempts = 0;
	};
	TMap<FGuid, FCrowdyPendingBindBackoff> PendingBindBackoff;

	// The one armed sweep. While it is pending, every further notification is absorbed rather than sweeping again.
	FTimerHandle PendingSweepTimer;

	// Containers whose model-changed notifications are waiting to be pulled, and the local entity holding each
	// one. Keyed on the container, so however many notifications one container produces inside a window it is
	// pulled once; the value is REPLACED by every notification, so the pull addresses whichever entity holds the
	// container at the latest one rather than the one that held it at the first. A container rebound mid-window
	// would otherwise have its owner's values applied onto the entity that has let it go.
	TMap<FString, FGuid> PendingRefreshPulls;

	// The one armed drain. While it is pending, every further notification joins the map instead of arming again.
	FTimerHandle RefreshPullTimer;

	// How long the drain should actually wait, given how much of the call allowance the last window's worth of
	// calls already spent. A pull is a Game API call and is counted in the same ledger an invoke is, so a window
	// that never widened would emit pulls at a fixed rate into an allowance those very pulls are spending, and
	// the first sign of trouble would be the server refusing rather than this client merging harder.
	float ResolveRefreshPullWindowSeconds() const;

	// Records a notification for a bound container and arms the drain. Never pulls inline: the coalescing is the
	// whole point, and one code path owning the pull is what keeps a fight from spending the call allowance.
	void EnqueueRefreshPull(const FString& ContainerId, const FGuid& NetID);

	// Pulls each waiting container once and clears the map.
	void DrainRefreshPulls();

	// How many container resolves have got past the already-bound and in-flight guards, which is where a bind
	// starts spending the Game API allowance. A diagnostic; it is never read to make a decision.
	int32 ContainerResolveStartCount = 0;

	// How many sweeps have run. A diagnostic (and the thing a test counts to prove that notification rate no longer
	// decides round-trip rate); it is never read to make a decision.
	int32 PendingSweepCount = 0;

	// How many times a holder has asked for an entity's component containers to be stood in for. A diagnostic, and
	// what a test counts to prove that minting a participant does not ask and the bind door does; it is never read
	// to make a decision.
	int32 DerivedEnrollmentRequestCount = 0;

	// True when every sub-participant recorded for this anchor still has a live stand-in, so the derivation has
	// nothing to add. Lookups only, because it gates the per-call cost of the enrolment path.
	bool IsAnchorFullyStoodIn(const UCrowdyEntitySubsystem& Entities, const FGuid& AnchorNetID) const;

	// Anchor actor NetID -> the sub-participant NetIDs enrolled for its CrowdyContainer components. Populated by
	// RegisterComponentSubParticipants and drained in HandleEntityUnregistered so an anchor's teardown unregisters
	// its component sub-participants (each then clears its own binding/cache through its own unregistration).
	TMap<FGuid, TArray<FGuid>> SubParticipantsByAnchor;

	// Sub-participant NetID -> the stand-in object enrolled under it. A UPROPERTY map because these are the only
	// references keeping the stand-ins alive: the registry holds its participant weakly, and an Outer is an edge
	// from the inner to the outer and never the reverse, so a collection would take the stand-in while its record
	// and its container binding survived it. The entity would then look bound while every pull for it landed on
	// nothing, and the surviving record would bar a replacement from ever being minted.
	// Emptied for an id in HandleEntityUnregistered, beside the rest of that id's state.
	UPROPERTY()
	TMap<FGuid, TObjectPtr<UCrowdyContainerStandIn>> ContainerStandIns;

	// The entity subsystem whose OnEntityRegistered/Unregistered we subscribe to for auto-bind. A raw pointer to a
	// same-world UWorldSubsystem (our exact lifetime); nulled in Deinitialize before teardown so a late broadcast
	// never lands on us.
	UCrowdyEntitySubsystem* EntitySubsystemForEvents = nullptr;

	// The sole OnModelChanged consumer: route a normalized hint to the existing re-pull. Prefers the container
	// (HandleModelChangedByContainer) when present, else the entity (HandleModelChanged) - the same precedence
	// the ping carrier used before the seam existed. Bound to OnModelChangedDelegate in Initialize.
	void HandleModelChangeHint(const FCrowdyModelChangeHint& Hint);

	// Self-echo drop state. When THIS client invokes a container it applies the confirmed mutationsApplied
	// directly; the server-native model-changed echo (channel or 139) then arrives for the same container.
	// Re-pulling on that echo is redundant and, for a free/data container, races the just-cached value
	// (ApplyDataContainerState full-replaces on the pull, transiently evicting it). MarkSelfActed records each
	// self-invoked container briefly; ConsumeSelfEcho drops its first echo. Mirrors CrowdyState's self-echo drop,
	// which keys on the originating sender id; a channel notification carries no reliable sender, so this keys on
	// recency instead. Game-thread only. The two helpers are public purely so the consume-once/keying semantics
	// are headless-testable (mirrors DecodeContainerIdFromState); the map stays private.
	TMap<FString, double> RecentlySelfActed; // container id -> expiry (FApp::GetCurrentTime seconds)

	// Drop every RecentlySelfActed record whose window has closed. Called from both MarkSelfActed and
	// ConsumeSelfEcho so the map stays bounded by the window even in a session that only ever invokes (never
	// receives a model-changed notification to drive a consume).
	void PruneExpiredSelfActed(double Now);

	// Idempotently subscribe this subsystem to the three model-changed carriers (opcode 139 server-native event,
	// opcode 138 fallback ping, opcode 18 channel). The bridge's service registry is created in the SDK
	// subsystem's post-world-init pass, which can run AFTER this world subsystem's Initialize, so a single
	// Initialize-time attempt can silently no-op and leave server-native notifications unheard. Called from
	// Initialize, OnWorldBeginPlay, and the entity-bind path; the flag makes repeat calls a no-op so a retry never
	// takes a second, redundant subscription (which would double-fire every OnRep).
	void EnsureReceptionLayerRegistered();

	// Claims the three model-changed carriers on Registry and points them at HandleModelChangedDelivery. Exactly
	// one subsystem holds that claim per registry: the registry is game-instance scoped and outlives every world
	// under it, so subscribing here releases the claim held by whichever world subscribed before this one.
	void SubscribeToModelChangedCarriers(FCrowdyServiceRegistry& Registry);

	// Drops this subsystem's carrier subscription and lets EnsureReceptionLayerRegistered take a fresh one later.
	// Used both on teardown and when a newly entered world takes the claim over from this one.
	void ReleaseReceptionLayer();

	// The delivery callback for the three-carrier subscription: decodes whichever carrier this is and hands
	// the normalized hint straight to the notification sink, which is what triggers the re-pull.
	void HandleModelChangedDelivery(const FCrowdyDelivery& Delivery);

	// The SDK bridge whose service registry we subscribe through. Raw (mirrors UCrowdyEntitySubsystem); cleared
	// on Deinitialize.
	UCrowdySDKBridgeSubsystem* Bridge = nullptr;

	// One handle over all three model-changed carriers; released on Deinitialize.
	FCrowdySubscription ModelChangedSubscription;

	// True once EnsureReceptionLayerRegistered has actually subscribed, so the retries never subscribe twice.
	bool bReceptionLayerRegistered = false;

	// Set when a newer world took the carrier subscription over from this one. Latched, so a departed world can
	// never re-subscribe through one of EnsureReceptionLayerRegistered's other call sites and silence the world
	// the player is actually in.
	bool bSupersededByNewerWorld = false;

	// The one delegate every model-changed carrier broadcasts to (ICrowdyModelNotificationSink). Bound to
	// HandleModelChangeHint in Initialize; it dies with the subsystem, so there is no dangling subscriber.
	FOnModelChanged OnModelChangedDelegate;

	// Free/data container state addressed by containerId (no NetID, no actor): containerId -> (server key ->
	// canonical JSON), mirroring ContainerCache. Diffed on pull to drive OnDataContainerChanged.
	TMap<FString, TMap<FName, FString>> DataContainerCache;
	// Containers watched for model-changed notifications (pulled/created/invoked/explicitly watched), so a
	// notification for one re-pulls it. Actor-bound containers are found via NetIDToContainerId instead.
	TSet<FString> WatchedDataContainers;

	// Everything two applies must agree on before they may be merged into one invoke. Every string part is reduced to
	// a hash so the key costs no allocation on a path that runs many times a second, and so the timer that closes a
	// window can carry its own key by value.
	//
	// The target parts are the RESOLVED ones the dispatch will use, not the caller's raw request: the container id an
	// entity-bound apply resolved to and the epoch of that binding, rather than the entity id alone, and the session
	// after the active-session fallback rather than the caller's (possibly empty) one. Keying on the raw values would
	// let two applies that resolve differently share a window, and the whole summed magnitude would then land wherever
	// the resolution happened to point when the window closed.
	struct FCrowdyCoalesceKey
	{
		FGuid SelfNetID;
		uint64 ContainerIdHash = 0;
		uint64 FunctionHash = 0;
		uint64 SessionHash = 0;
		uint64 AccumulateHash = 0;
		uint64 Discriminator = 0;
		uint32 BindEpoch = 0;
		bool bEntityBound = false;

		bool operator==(const FCrowdyCoalesceKey& Other) const
		{
			return bEntityBound == Other.bEntityBound
				&& BindEpoch == Other.BindEpoch
				&& SelfNetID == Other.SelfNetID
				&& ContainerIdHash == Other.ContainerIdHash
				&& FunctionHash == Other.FunctionHash
				&& SessionHash == Other.SessionHash
				&& AccumulateHash == Other.AccumulateHash
				&& Discriminator == Other.Discriminator;
		}

		// The integral members are qualified so each resolves to the engine's own global overload rather than to
		// this function while it is being defined. FGuid's hash is a hidden friend, reachable only by
		// argument-dependent lookup, so that one call has to stay unqualified.
		friend uint32 GetTypeHash(const FCrowdyCoalesceKey& Key)
		{
			uint32 Hash = GetTypeHash(Key.SelfNetID);
			Hash = HashCombine(Hash, ::GetTypeHash(Key.ContainerIdHash));
			Hash = HashCombine(Hash, ::GetTypeHash(Key.FunctionHash));
			Hash = HashCombine(Hash, ::GetTypeHash(Key.SessionHash));
			Hash = HashCombine(Hash, ::GetTypeHash(Key.AccumulateHash));
			Hash = HashCombine(Hash, ::GetTypeHash(Key.Discriminator));
			Hash = HashCombine(Hash, Key.BindEpoch);
			return HashCombine(Hash, Key.bEntityBound ? 1u : 0u);
		}
	};

	// One merge window: the invoke that will be sent when it closes, plus every caller waiting on its outcome.
	struct FCrowdyCoalescedInvoke
	{
		bool bEntityBound = false;
		FGuid SelfNetID;
		FString ContainerId;
		FString FunctionName;
		FString SessionId;

		// The binding this window was opened against, re-checked before the merged invoke is sent. A window whose
		// entity has since unbound, or rebound (the same container row can serve a later life of the same entity), is
		// failed rather than dispatched, so a previous life's summed magnitude never lands on the current one.
		uint32 BindEpoch = 0;

		// The order this window opened in, used to pick which window to close early when the coalescer is at capacity.
		// The oldest is the one closest to flushing anyway, so closing it costs the least added latency. A counter
		// rather than a timestamp: windows opened in the same frame all read one clock value, which would make
		// "oldest" mean "whichever the map happened to visit first".
		uint64 OpenSequence = 0;

		// The first apply's parameters, copied. Every key but AccumulateParam is already the value the whole window
		// agrees on, because a differing one would have keyed a different window; AccumulateParam is overwritten with
		// the running sum just before the invoke is sent.
		TSharedPtr<FJsonObject> Params;
		FString AccumulateParam;
		double AccumulatedValue = 0.0;
		bool bAccumulateIsInteger = false;
		int32 MergedCount = 0;

		// Every caller merged into this window, in arrival order. Each gets its own copy of the one outcome; a null
		// entry is a fire-and-forget caller and is skipped, never treated as a missing waiter.
		TArray<TFunction<void(FCrowdyInvokeResult)>> Waiters;

		FTimerHandle Timer;
	};

	// One invoke waiting out a backoff before its retry. Held rather than left to the timer alone, so a world torn
	// down mid-backoff can still tell every waiting caller what happened instead of stranding it on a timer manager
	// that is about to go away.
	// The entity binding an invoke was dispatched under, carried far enough down that a RETRY can prove the binding
	// still holds before re-sending. A container-addressed invoke leaves it unbound, because it names its target
	// directly and has no binding that could move under it.
	//
	// It exists because a container id is a deterministic function of the NetID, so an entity that unbinds and binds
	// again resolves to the SAME row while being a different life. The coalescer already refuses to flush a merged
	// window across that boundary; a retry re-sends an already-flushed request seconds later, well past the window's
	// own lifetime, so it has to make the same check or it walks around the one that exists.
	struct FCrowdyInvokeBindingGuard
	{
		bool bEntityBound = false;
		FGuid SelfNetID;
		uint32 BindEpoch = 0;
	};

	struct FCrowdyPendingInvokeRetry
	{
		FTimerHandle Timer;
		FCrowdyInvokeResult LastResult;
		TFunction<void(FCrowdyInvokeResult)> OnDone;
		FCrowdyInvokeBindingGuard Guard;
	};

	// Open merge windows, keyed by everything that has to match for two applies to merge (see FCrowdyCoalesceKey).
	TMap<FCrowdyCoalesceKey, FCrowdyCoalescedInvoke> PendingCoalesced;

	// Stamped onto each window as it opens, so "the oldest open window" is answerable without a clock.
	uint64 NextCoalesceOpenSequence = 1;

	// Invokes waiting out a budget backoff, keyed by an id the timer carries so a fired timer can claim its own
	// record and a teardown can fail whatever is left.
	TMap<uint64, FCrowdyPendingInvokeRetry> PendingInvokeRetries;
	uint64 NextInvokeRetryId = 1;

	// When each Game Model call this client made was dispatched, ascending, as a fixed-capacity ring: InvokeLedgerHead
	// is the oldest live entry and InvokeLedgerCount how many there are. The sliding-window ledger the governor reads
	// to decide how much of the allowance is spent. A ring rather than an array that shifts, because recording an
	// attempt is on the path of every call and dropping expired entries off the front of an array memmoves the rest of
	// it every time. Past the capacity the answer is already "the allowance is spent", so the oldest entry is
	// overwritten rather than the ring grown.
	static constexpr int32 InvokeLedgerCapacity = 4 * InvokeBudgetLimitPerWindow;
	TArray<double> InvokeLedger;
	int32 InvokeLedgerHead = 0;
	int32 InvokeLedgerCount = 0;

	// Latched at the top of Deinitialize, before anything is drained. A drained caller's completion can re-enter and
	// try to apply again; a window or backoff armed after this point would sit on a timer manager that is about to be
	// destroyed and would never fire, stranding its caller. With the latch set, a late apply is dispatched or failed
	// on the spot instead of being queued.
	bool bShuttingDown = false;

	// Whether the binding this invoke was dispatched under is still the one in place. Vacuously true for an unbound
	// (container-addressed) invoke, which has nothing to move.
	bool IsInvokeBindingStillValid(const FCrowdyInvokeBindingGuard& Guard) const;

	// Invoke with the session ALREADY resolved, so nothing downstream re-applies the active-session fallback. The
	// request is held by shared reference from here on: one invoke can outlive two callbacks and a backoff, and
	// copying the whole request into each of them only to move a struct around cost a handful of string allocations
	// per call. Nothing mutates it after this point except the per-attempt app id.
	void InvokeResolved(TSharedRef<FCrowdyInvokeRequest> Resolved, TFunction<void(FCrowdyInvokeResult)> OnDone,
		const FCrowdyInvokeBindingGuard& Guard = FCrowdyInvokeBindingGuard());

	// InvokeAndApply / InvokeOnContainer with the session already resolved. The public entries resolve it and defer to
	// these; the coalescer calls them directly with the session it pinned when the window opened.
	void InvokeAndApplyResolved(const FGuid& SelfNetID, const FString& FunctionName,
		const TSharedPtr<FJsonObject>& Params, const FString& ResolvedSessionId,
		TFunction<void(FCrowdyInvokeResult)> OnDone);
	void InvokeOnContainerResolved(const FString& ContainerId, const FString& FunctionName,
		const TSharedPtr<FJsonObject>& Params, const FString& ResolvedSessionId,
		TFunction<void(FCrowdyInvokeResult)> OnDone);

	// The single gameModelInvoke dispatch: resolve the API context, send it, and either retry a budget refusal or hand
	// the outcome to OnDone. InvokeResolved calls this with attempt 0; a retry calls it again with the SAME request
	// object so the retried call is byte-identical.
	void DispatchInvokeAttempt(TSharedRef<FCrowdyInvokeRequest> Resolved, int32 AttemptIndex,
		TFunction<void(FCrowdyInvokeResult)> OnDone, const FCrowdyInvokeBindingGuard& Guard);

	// Arm a backoff and retry Resolved when Result is a rate-limit refusal and there is retry budget left. True when
	// a retry was armed, in which case OnDone belongs to the retry and the caller must NOT complete it; false when
	// the caller owns the outcome and must complete OnDone itself. Guard is re-checked when the timer fires, not here:
	// the binding can move at any point during the backoff, so the only reading that means anything is the last one.
	bool TryScheduleBudgetRetry(const TSharedRef<FCrowdyInvokeRequest>& Resolved, int32 AttemptIndex,
		const FCrowdyInvokeResult& Result, const TFunction<void(FCrowdyInvokeResult)>& OnDone,
		const FCrowdyInvokeBindingGuard& Guard);

	// Record one dispatched Game API call in the sliding-window ledger, and drop everything that has aged out of it.
	void RecordInvokeAttempt(double Now);

	// Bind / unbind an entity's container id, keeping the reverse index and the bind epoch in step. Every write to
	// NetIDToContainerId goes through these two, so nothing can add a binding the reverse index does not know about.
	void AddContainerBinding(const FGuid& NetID, const FString& ContainerId);
	void RemoveContainerBinding(const FGuid& NetID);

	// The local NetID bound to ContainerId, or an invalid guid when nothing here is bound to it: one hash lookup for
	// the single holder a row normally has, and where two distinct local entities end up on one row (an explicit
	// bind, or two class-derived read-only bindings) the newest that still resolves, so every by-id path agrees.
	FGuid FindNetIDForContainer(const FString& ContainerId) const;

	// Close one merge window: take the record out, then send it. Taking it out FIRST is what makes a re-entrant apply
	// (a waiter's own handler applying the same effect again) open a fresh window instead of joining one mid-flush.
	void FlushCoalescedInvoke(const FCrowdyCoalesceKey& Key);

	// Close whichever open window has been open longest, to free a slot when the coalescer is at capacity. A no-op
	// when nothing is open.
	void FlushOldestCoalesceWindow();

	// Send a closed window's merged invoke and fan its single outcome out to every waiter. A window whose entity has
	// unbound or rebound since it opened is failed here instead, so its merged magnitude is never applied to a target
	// it was not authored against.
	void DispatchCoalescedInvoke(FCrowdyCoalescedInvoke&& Pending);

	// Send one request on its own, unmerged, down whichever route it names, against the session the enqueue resolved.
	void DispatchUncoalescedInvoke(const FCrowdyCoalesceRequest& Request, const FString& ResolvedSessionId,
		TFunction<void(FCrowdyInvokeResult)> OnDone);

	// Teardown drains. Each fails every caller still waiting rather than letting a dead timer swallow the callback:
	// a coalesced apply that was never sent is honestly a failure, and a retry that never ran reports the refusal
	// that would have caused it.
	void FailPendingCoalesceWindows();
	void FailPendingInvokeRetries();

	// Marks this world's Game Model session as live. The API client is owned by the game instance and outlives this
	// world subsystem, so a request issued here can complete after the world has torn down. Dropping this token in
	// Deinitialize is what makes ResolveLive stop handing out the subsystem, so a late completion still runs and
	// still notifies its caller but never writes into the caches and delegates already cleared.
	TSharedPtr<uint8> WorldSessionToken;

#if !UE_BUILD_SHIPPING
	// The diagnostic container-change feed, held as the bare handle id rather than the handle type: no bridge type
	// belongs in a public header here, and the id is all that is needed to close it again.
	uint64 DebugContainerWatchId = 0;
#endif
};
