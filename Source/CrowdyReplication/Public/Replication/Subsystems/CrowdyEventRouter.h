// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Core/FCrowdyTypeID.h"
#include "Core/UDP/Enums/ECrowdyTarget.h"
#include "Core/UDP/Structures/FCrowdyEventContext.h"
#include "Core/UDP/Subscription/FCrowdySubscription.h"
#include "Messages/GameObjects/FCrowdyEntitySpawnEvent.h"
#include "Replication/State/CrowdyStateCodec.h"
#include "Replication/Subsystems/ICrowdyEntitySubscriber.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectArray.h"
#include "CrowdyEventRouter.generated.h"


class FCrowdyServiceRegistry;
class FProperty;
class UCrowdySDKBridgeSubsystem;
class UCrowdyEntitySubsystem;
class UCrowdyAutoRegistry;
class UCrowdyStateReplicator;
struct FCrowdyFnInfo;
struct FCrowdyRepLayout;
struct FCrowdyRpcCall;
struct FCrowdyStateDelta;
struct FCrowdyDelivery;

/**
 * Ceilings and budgets for the queue of events waiting on an entity that has not registered yet. Every
 * one of these bounds untrusted input: an inbound event names an entity id chosen by whoever sent it, so
 * nothing about the queue may grow with what arrives. Exposed so a test states the same numbers the
 * router enforces instead of copying them.
 */
namespace CrowdyDeferQueue
{
	// Hard ceiling on the whole queue. Oldest is evicted first: the newest event is the one most likely
	// to still matter by the time its entity appears.
	inline constexpr int32 MaxEvents = 1024;

	// Separate ceiling on how many DISTINCT entity ids the queue waits on at once. A total count alone
	// does not stop the cheap attack, which is one event each for a million invented ids: that fits
	// under any per-id limit while filling the queue and evicting every legitimate wait. Oldest id
	// evicted first, together with every event held for it.
	//
	// Both numbers leave room for the legitimate burst that is easy to forget: joining a busy area
	// delivers events for many entities at once, some of them ahead of the message that creates the
	// entity. A ceiling tuned only against the attack drops those, and it drops them quietly.
	inline constexpr int32 MaxDistinctEntities = 256;

	// Wall-clock budget for a one-shot action (a swing, a flinch, an emote). An action is over in a
	// fraction of a second, so an action replayed after this has elapsed is not late, it is wrong: the
	// entity it names may have died and respawned under the same id in between. Deliberately measured
	// in time rather than in ticks, because a tick budget means whatever the frame rate happens to be.
	inline constexpr double ActionBudgetSeconds = 0.3;
}

/**
 * A game event as received off the wire, with its envelope.
 *
 * Payload is BORROWED, never owned: it points at the decoded struct the wire message is still holding,
 * or at a caller's own struct on the non-wire entry points. Whatever it points at must outlive the
 * dispatch call, which every caller satisfies by keeping it on the stack for the duration. Nothing may
 * store this struct past that call; the wait queue copies the payload it holds (see FCrowdyDeferredEvent).
 */
struct FCrowdyInboundEvent
{
	const FInstancedStruct* Payload = nullptr;
	FGuid SenderID;
	ECrowdyTarget Target = ECrowdyTarget::Everyone;
	FGuid TargetID;

	// True when this arrived as a SINGLE_ACTOR_MESSAGE (the server already delivered it only to
	// the target's owner), false for a spatial broadcast. The RPC receive path uses this to skip
	// the broadcast-only echo-drop and authority check.
	bool bTargetedDelivery = false;

	// FPlatformTime::Seconds at which this event reached the router, stamped once at the entry point it
	// arrived through. Age is measured from here, so an event that waits, retries and waits again is
	// judged on how long it has existed rather than on how long since its last retry, which would slide
	// the deadline forward forever. The wire carries no send timestamp, so arrival here is the earliest
	// moment this client can attribute to the event; a delivery delay on the way in counts as zero age.
	// Zero means unstamped, and the first defer stamps it then.
	double ReceivedAtSeconds = 0.0;
};

/**
 * An inbound event (an RPC call or a state delta) whose target entity has not registered yet, held for
 * a bounded number of retries so an event that races ahead of its entity's spawn event still applies
 * once the entity registers.
 */
struct FCrowdyDeferredEvent
{
	/**
	 * The only storage the queued payload has. Waiting outlives the delivery that carried the event, so
	 * the queue owns a copy rather than pointing at bytes the transport has since reused.
	 *
	 * Event::Payload is deliberately left null while the entry sits here: the queue relocates its elements
	 * on every eviction and on every growth, so an entry pointing at its own field would be reading a
	 * moved-from address. A retry builds a fresh view over this at dispatch time instead.
	 */
	FInstancedStruct OwnedPayload;

	FCrowdyInboundEvent Event;
	int32 RemainingAttempts = 0;

	// Absolute time (FPlatformTime::Seconds) past which this event is evicted rather than retried, or
	// zero for no deadline, which leaves the retry count as the only limit. Derived from the event's own
	// arrival stamp, so re-deriving it on a re-defer produces the same instant.
	double ExpiresAtSeconds = 0.0;
};

/**
 * One decode buffer per replicated class, shared by every entity of that class whose state is applied
 * without an object to apply it to (see ICrowdyEntitySubscriber). Deliberately NOT a USTRUCT: it holds
 * a raw allocation plus raw FProperty pointers, neither of which has a wire form or is ever serialized.
 *
 * The buffer is shaped like an instance of the class (the class's own properties size and alignment) but no
 * instance is ever constructed: the state codec addresses a value as base plus the property's offset, which
 * a plain allocation satisfies. Constructing one would build components and subobjects, and an actor class
 * constructed with no world is a trap, so only the properties the layout actually names are initialized.
 *
 * This is sound only because a CrowdyState rep layout is restricted to POD leaves and plain USTRUCTs, and
 * rejects object references, interfaces, delegates and containers (see ECrowdyStatePropertySupport). A value
 * in the layout therefore never points at anything that has to be constructed or resolved for it to be
 * readable. The day that restriction widens, an uninitialized buffer stops being a safe stand-in for an
 * instance and this has to be revisited.
 *
 * The properties named by the layout ARE initialized, because a layout may name an FString or another leaf
 * that owns heap, and a value decoded into uninitialized memory would be read as a garbage allocation. Each
 * one is destroyed exactly once, through InitializedProps rather than through the registry's layout: the
 * registry frees and rebuilds its layouts on a rescan, while these UClass-owned properties survive one.
 */
struct FCrowdyStateScratchContainer
{
	// The class the buffer is shaped like; weak so a cached buffer never keeps a class alive, and so a
	// class that went away is detectable before its properties are dereferenced.
	TWeakObjectPtr<const UClass> OwnerClass;

	// Hash of the layout the buffer was initialized against. A layout whose hash has moved describes a
	// different set of slots, so the buffer is torn down and rebuilt rather than reused under it.
	int64 LayoutHash = 0;

	// This class's wire id, resolved once when the buffer is built rather than per delta. Deriving it is not
	// free: it formats the class's full path name into a string and interns it as an FName before the lookup,
	// which is real work to recompute for a value that cannot change while the class is loaded. Cached here
	// because this buffer is already the one thing kept per class.
	uint32 ClassID = 0;

	// Raw allocation of the class's properties size, at the class's minimum alignment. Never resized.
	uint8* Data = nullptr;
	int32 Size = 0;

	// The properties initialized into Data, snapshotted at build time so teardown never has to consult a
	// layout that may since have been freed and rebuilt.
	TArray<const FProperty*> InitializedProps;

	FCrowdyStateScratchContainer() = default;

	// Non-copyable and non-movable: it owns one allocation plus initialized values (an FString's heap, for
	// example) that must be destroyed exactly once. It is held behind a TUniquePtr, so neither is needed.
	FCrowdyStateScratchContainer(const FCrowdyStateScratchContainer&) = delete;
	FCrowdyStateScratchContainer& operator=(const FCrowdyStateScratchContainer&) = delete;
	FCrowdyStateScratchContainer(FCrowdyStateScratchContainer&&) = delete;
	FCrowdyStateScratchContainer& operator=(FCrowdyStateScratchContainer&&) = delete;

	// Out of line in the .cpp: it needs the full FProperty definition to destroy the initialized values.
	~FCrowdyStateScratchContainer();
};

/**
 * Receives game events off the wire and routes them to registered handler
 * objects according to the event envelope (see ECrowdyTarget).
 */
UCLASS()
class CROWDYREPLICATION_API UCrowdyEventRouter : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// Routes the event to handlers according to its envelope (Target/TargetID)
	void DispatchEvent(const FCrowdyInboundEvent& Event);

	// Debug loopback (crowdy.rpc.loopback): runs a call this client just sent through its own
	// receive path as a plain Everyone-addressed announcement, so the serialize/resolve/dispatch
	// round-trip can be exercised with a single client. FCrowdyRPC guards against re-entry.
	void ReceiveLoopbackCall(const FCrowdyRpcCall& Call);

	// Debug loopback (crowdy.state.loopback): replays a CrowdyState delta the UCrowdyStateReplicator just
	// sent onto a distinct local "mirror" entity (a second instance of the same class, RemoteProxy role,
	// a different NetID), so a single PIE client can exercise decode + OnRep + the receive-side gates
	// without a second client. Unlike ReceiveLoopbackCall this cannot replay onto the sending entity
	// itself: DispatchStateDelta's ownership gate drops a non-host-sourced delta for an entity we own, and
	// decoding onto the same live actor that produced the delta would report zero changed properties. The
	// caller (UCrowdyStateReplicator) is responsible for cloning the outgoing delta, overwriting EntityID
	// with the mirror's NetID, and clearing SenderID before calling this; DispatchStateDelta itself is
	// unmodified.
	void ReceiveLoopbackStateDelta(const FCrowdyStateDelta& Delta);

	// Receive entry for a reliable RPC delivered over the session channel. UCrowdyChannels decodes
	// the channel payload and calls this; the call is dispatched like a spatial Multicast (the sender's
	// own echo is dropped by SenderID, the body runs once on every other member) before this returns.
	// Game thread only.
	void ReceiveChannelRpcCall(const FCrowdyRpcCall& Call);

	// Receive entry for a CrowdyState delta delivered over the session channel (the non-spatial transport for
	// subsystem participants). UCrowdyChannels discriminates the channel payload by its leading kind tag,
	// decodes it via DecodeChannelStateDelta, and calls this; the delta is dispatched like a spatial
	// Multicast through the unchanged DispatchStateDelta (self-echo dropped by SenderID, applied on every
	// other member) before this returns. Game thread only.
	void ReceiveChannelStateDelta(const FCrowdyStateDelta& Delta);

	// UTickableWorldSubsystem. Drives the retry ladder for events that arrived ahead of their target
	// entity's spawn; inbound routing itself is not tick-driven.
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(UCrowdyEventDispatcher, STATGROUP_Tickables);
	}

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	// Resolves the object a state delta applies to: the actor itself when it IsA the layout's owner
	// class, else its first component that IsA it (deterministic GetComponents() order), else null.
	// Mirrors the file-static ResolveRpcReceiver but keys on the layout's UClass; public+static so the
	// container-resolution cases are directly unit-testable without a world.
	static UObject* ResolveStateContainer(AActor* Actor, const UClass* LayoutOwnerClass);

	// UObject participant overload: the participant itself when it IsA the layout's owner class, else (when it is
	// an actor) its first component that IsA it, else null. The AActor* overload forwards here.
	static UObject* ResolveStateContainer(UObject* Participant, const UClass* LayoutOwnerClass);

	/**
	 * Registers the subscriber the router hands everything addressed to an entity that is in no actor-side
	 * registry (see ICrowdyEntitySubscriber). One subscriber at a time, and the most recent registration
	 * holds it.
	 *
	 * Every plane goes through this one registration, because an entity that lives as a row rather than as
	 * an object is the same entity on all of them: whoever holds its storage holds it for calls, for view
	 * state and for model values alike. A subscriber with nothing to do on a plane refuses there, which is
	 * a different answer from not holding the id at all.
	 *
	 * Two worlds exist at once while travelling between levels, and both run this code. Whichever
	 * registers last is the world the player is now in, so it takes the slot and the earlier one is
	 * marked as displaced. A displaced subscriber can then neither take the slot back nor release it,
	 * which is the point: the world being left behind must not silence the world being entered, and it
	 * would otherwise do exactly that by tearing down and unregistering after the new one arrived.
	 */
	void RegisterEntitySubscriber(TScriptInterface<ICrowdyEntitySubscriber> Subscriber);

	// Releases the subscriber registered above. A no-op unless Subscriber is the one currently holding the
	// slot, so a departing world releases nothing but its own claim.
	void UnregisterEntitySubscriber(const TScriptInterface<ICrowdyEntitySubscriber>& Subscriber);

	// The subscriber currently holding the slot, or an empty interface when none does.
	//
	// Exposed so that a plane whose values are settled elsewhere (server-owned model values, which arrive
	// through a pull rather than through this router) can deliver to the SAME subscriber this router already
	// holds. Keeping one slot matters more than it looks: a second registration for the same subscriber is a
	// second thing to keep in step, and the way it fails is silent, because a path that claims one slot and
	// not the other leaves some planes working while another reaches nothing at all.
	const TScriptInterface<ICrowdyEntitySubscriber>& GetEntitySubscriber() const { return EntitySubscriber; }

	// Test seams (headless, no world/bridge): inject the collaborators Initialize would resolve and drive
	// the deferred-retry pass. DispatchEvent is already public, so a test feeds an inbound event straight
	// through it.
	void SetEntitySubsystemForTest(UCrowdyEntitySubsystem* In) { EntitySubsystem = In; }
	void SetAutoRegistryForTest(UCrowdyAutoRegistry* In) { AutoRegistry = In; }
	void RetryDeferredForTest() { RetryDeferredEvents(); }
	int32 NumDeferredForTest() const { return DeferredEvents.Num(); }

	// How many distinct entity ids the queue is waiting on, which is the quantity the distinct-id cap
	// bounds; the total event count alone cannot show that cap working.
	int32 NumDeferredEntitiesForTest() const;

	// Running total of events dropped by any eviction path (over a cap, or past a deadline), so a test
	// can tell an event that was evicted from one that was never queued.
	int32 NumDeferredEvictionsForTest() const { return DeferredEvictionCount; }

	// How many calls were dropped because the subscriber holds the entity as data and nothing is
	// registered to apply the function to it, and how many of those were reported. The pair is the point:
	// the first counts every drop and the second must stay at one per function however many arrive, which
	// is what says the report is throttled rather than either absent or repeated per event.
	int32 NumUnhandledSubscriberEventsForTest() const { return UnhandledSubscriberEventCount; }
	int32 NumUnhandledSubscriberEventReportsForTest() const { return UnhandledSubscriberEventReportCount; }

	// How many calls were dropped because nothing on this client could receive them, and how many of those
	// were reported. Same pairing as above and for the same reason: every drop is counted, while the line
	// is rate-gated, so a throttled report and no traffic at all can be told apart.
	int32 NumUndeliverableCallsForTest() const { return UndeliverableCallCount; }
	int32 NumUndeliverableCallReportsForTest() const { return UndeliverableCallReportCount; }

	// How many calls were dropped before decode because the receiver's inbound allowance was spent. Only
	// ever logged under the RPC trace, so on a quiet build this counter is the whole account of it.
	int32 NumBudgetRefusedCallsForTest() const { return BudgetRefusedCallCount; }

	// Injects the replicator the apply path adopts host corrections into (see AdoptHostValues). In production
	// the router resolves it from the world; a headless apply test has no world, so it injects one directly.
	void SetStateReplicatorForTest(UCrowdyStateReplicator* In) { StateReplicatorForTest = In; }

#if WITH_DEV_AUTOMATION_TESTS
	// Takes the event fallback subscription against a registry a test owns, using the same call Initialize
	// makes, so a test drives the production delivery path rather than a copy of it.
	void SubscribeToEventFallbackForTest(FCrowdyServiceRegistry& Registry) { SubscribeToEventFallback(Registry); }
#endif

private:

	UCrowdySDKBridgeSubsystem* Bridge = nullptr;
	UCrowdyEntitySubsystem* EntitySubsystem = nullptr;
	UCrowdyAutoRegistry* AutoRegistry = nullptr;
	UCrowdyStateReplicator* StateReplicatorForTest = nullptr;

	// The router is the Fallback for every event type no other subscriber Handles; released on Deinitialize.
	FCrowdySubscription EventFallbackSubscription;

	// Claims every event payload nothing else Handles on Registry and points them at HandleDelivery.
	void SubscribeToEventFallback(FCrowdyServiceRegistry& Registry);

	// Delivery callback for the fallback subscription: builds the envelope and routes it through
	// DispatchEvent before returning.
	void HandleDelivery(const FCrowdyDelivery& Delivery);

	// Inbound events awaiting their target entity's spawn (see FCrowdyDeferredEvent). Insertion order is
	// age order, which is what every eviction path reads to find the oldest.
	TArray<FCrowdyDeferredEvent> DeferredEvents;

	// Receives calls, state and model values for entities that are in no actor-side registry. Held as a
	// TScriptInterface so the subscriber object is kept alive and reachable for as long as the router
	// points at it.
	UPROPERTY()
	TScriptInterface<ICrowdyEntitySubscriber> EntitySubscriber;

	// Subscribers this router has already displaced, one per world left behind. A displaced subscriber is
	// refused both registration and release, so a world being torn down cannot take the slot back from,
	// or empty it out from under, the world that succeeded it. Weak, and pruned on every registration,
	// so a collected world leaves nothing behind.
	TArray<TWeakObjectPtr<UObject>> DisplacedEntitySubscribers;

	// One decode buffer per class, built on first use and kept for the router's lifetime. Keyed by the raw
	// class pointer for a cheap lookup; every hit is validated against the entry's own weak class pointer,
	// so a collected class can never be answered with a buffer shaped for something else. Only classes the
	// registered subscriber names ever appear here, so this is bounded by what the project authored and
	// never grows with what arrives off the wire.
	TMap<const UClass*, TUniquePtr<FCrowdyStateScratchContainer>> StateScratchContainers;

	// The buffer for Class, built (or rebuilt, when the layout hash has moved under it) on demand. Returns
	// null when no buffer can safely be shaped for the class.
	FCrowdyStateScratchContainer* ResolveStateScratch(const UClass* Class, const FCrowdyRepLayout& Layout);

	// One codec decode scratch per layout, so a delta costs no allocation per present field. Kept apart from
	// the buffers above and keyed differently on purpose: those are decode TARGETS, shaped like a class and
	// keyed by the entity class, while these hold the value a byte is read into before it is compared against
	// a target. A decode whose target and scratch were the same allocation would compare every value with
	// itself, so the two can never come from one map.
	TMap<const UClass*, TUniquePtr<FCrowdyStateDecodeScratch>> StateDecodeScratches;

	// The decode scratch for Layout, built on first use and kept for the router's lifetime. Returns null when
	// the layout names no live class to build one against.
	FCrowdyStateDecodeScratch* ResolveDecodeScratch(const FCrowdyRepLayout& Layout);

	/**
	 * True at most once a second, for the drop reasons on the participant-less state path.
	 *
	 * Every one of those drops is per delta, and the situations that cause them are per CLASS rather than per
	 * entity: a sender on a drifted build, a class carrying no CrowdyState properties, a layout that failed to
	 * resolve. So the natural failure is not one line, it is one line per entity per change for every entity of
	 * that class, which at crowd scale costs more than the work it is reporting on. Reporting the first and
	 * then throttling keeps the diagnosis available without letting a misconfiguration become the outage.
	 */
	bool ShouldReportStateSubscriberDrop();

	// Wall-clock instant the throttle above reopens. Not serialized, not thread-shared: the state receive path
	// is game thread only.
	double NextStateSubscriberDropReportSeconds = 0.0;

public:

	/**
	 * Counters for the participant-less state path, so a gate can be judged on numbers rather than on what a
	 * corpse looks like. Every drop reason is separate on purpose: "nothing arrived" and "everything arrived
	 * and was refused for one specific reason" look identical from outside, and they call for opposite
	 * investigations. Diagnostic only, and never load-bearing for behaviour.
	 */
	struct FStateSubscriberStats
	{
		// Deltas the subscriber claimed, meaning it holds the id. Zero here with traffic on the wire means the
		// ids do not line up at all, not that the path is broken.
		int64 Claimed = 0;

		// Claimed deltas that reached the subscriber with a decoded buffer.
		int64 Applied = 0;

		// Total slots handed over across every applied delta. Applied above zero with this at zero is the
		// distinctive signature of a delta arriving and delivering nothing.
		int64 DeliveredSlots = 0;

		// Claimed deltas for an entity the subscriber holds with no class recorded for it. Ordinary rather
		// than faulty: an entity may exist as a position with nothing declared, and there is then no layout
		// to read the bytes against. Counted apart from the failures below because it is the one refusal here
		// that says nothing is wrong.
		int64 DroppedNoEntityClass = 0;

		int64 DroppedClassIdMismatch = 0;
		int64 DroppedNoLayout = 0;
		int64 DroppedNoScratch = 0;
		int64 DroppedDecodeFailed = 0;

		// Deltas that reached the subscriber branch for an id it does not hold, so they went on waiting
		// exactly as before. Not a failure by itself; a large number beside zero Claimed is the id-mismatch
		// signature.
		int64 NotClaimed = 0;
	};

	const FStateSubscriberStats& GetStateSubscriberStats() const { return StateSubscriberStats; }
	void ResetStateSubscriberStats() { StateSubscriberStats = FStateSubscriberStats(); }

	/**
	 * Counters for the drops taken on the state path for an entity that HAS a participant. Kept apart from the
	 * subscriber's counters rather than folded into them because these deltas were never the subscriber's to
	 * refuse: reading an authority drop beside FStateSubscriberStats would say a subscriber turned down
	 * something it was never offered.
	 *
	 * Every drop counted here is either throttled in the log or reported only under the state trace, so on a
	 * quiet build the counter is the only account of it that always exists. That is the whole point: a throttled
	 * or trace-gated drop and no traffic at all look identical from outside, and they call for opposite
	 * investigations. Diagnostic only, and never load-bearing for behaviour.
	 */
	struct FStateParticipantStats
	{
		// Deltas whose participant's class carries no rep layout and whose entity no subscriber holds either,
		// so there was nowhere to put them. The cause is an authoring fact about a class, so this climbing
		// steadily names one class rather than a transient fault.
		int64 DroppedNoLayoutNoSubscriber = 0;

		// Foreign non-host deltas for an entity this client owns. We are the authority for what we own, so this
		// is ordinary traffic being refused, not a fault by itself.
		int64 DroppedNotHostSourcedForOwned = 0;

		// Host corrections refused because the entity is authored OwnerOnly, which only its owner may write.
		int64 DroppedHostOverrideRefused = 0;

		// Non-host deltas for a locally host-owned world entity. Only the host writes world state, so anything
		// climbing here is some other peer trying to.
		int64 DroppedNotHostSourcedForWorld = 0;
	};

	const FStateParticipantStats& GetStateParticipantStats() const { return StateParticipantStats; }
	void ResetStateParticipantStats() { StateParticipantStats = FStateParticipantStats(); }

private:

	FStateSubscriberStats StateSubscriberStats;
	FStateParticipantStats StateParticipantStats;

	/**
	 * Offers a delta to the registered subscriber, for an entity with no participant and for one whose
	 * participant carries no rep layout and so cannot hold the state itself.
	 *
	 * Returns true when the delta is finished with here, which covers both delivery and every refusal: once
	 * the subscriber says it holds the id, the entity is one it holds as data and no amount of waiting can
	 * make a later attempt succeed. Returns false only when there is no subscriber or the subscriber does
	 * not hold the id, which is when the caller should go on waiting for the entity as it always has.
	 */
	bool DispatchStateDeltaToSubscriber(const FCrowdyStateDelta& Delta);

	// Reused across deltas so the receive path allocates nothing per entity once it is warm: the indices the
	// codec reports as changed, and the indices the delta actually carried. The present set is handed to the
	// subscriber by reference and stays live for the whole of that call, which may dispatch another delta, so
	// these are claimed by the outermost subscriber dispatch only.
	TArray<int32> StateDecodeChangedIndices;
	TArray<int32> StateDecodePresentIndices;

	// The same, for the participant path. That path reads its changed set twice, once to fire each notify and
	// again to adopt a host correction into the owner's shadow, and a notify may dispatch another delta on
	// this router in between. So the member is claimed by the outermost call only: a call that finds the flag
	// already set takes a local instead of writing into the set its caller is still using.
	TArray<int32> StateParticipantChangedIndices;

	// One flag per path, not one shared flag, because the two paths claim disjoint arrays: a subscriber
	// dispatch nested inside a participant one (or the reverse) never touches the array its caller is reading,
	// so making either push the other onto a local would cost an allocation for nothing.
	bool bDispatchingParticipantStateDelta = false;
	bool bDispatchingSubscriberStateDelta = false;

	// Running total of deferred events dropped by an eviction path, and when the last eviction line was
	// logged. Every eviction is counted and reported: a queue that silently shrinks looks exactly like a
	// queue that is working. The log line is rate-gated because eviction under a flood happens once per
	// arriving event, and a line each would hand the flood an amplifier; the count in the line covers
	// everything since the previous one.
	int32 DeferredEvictionCount = 0;
	int32 DeferredEvictionsSinceLog = 0;
	double LastEvictionLogSeconds = 0.0;

	// Counts NumEvicted dropped events and reports them under Reason, naming the entity they waited on.
	void ReportDeferredEviction(const TCHAR* Reason, const FGuid& EntityID, int32 NumEvicted);

	// Drops whole entities from the front of the queue until adding EntityID would leave it within the
	// distinct-id cap. Nothing happens when the queue is already waiting on that id.
	void EnforceDeferredEntityCap(const FGuid& EntityID);

	// Drops the oldest events until the queue has room for one more.
	void EnforceDeferredEventCap();

	// RPC-style CrowdyEvent receive path (FCrowdyRpcCall payloads): resolve the target
	// entity and UFunction, apply the client-authoritative ownership model, and invoke
	// the implementation. A participant that declares nothing able to receive the function
	// falls through to the registered subscriber rather than dropping the call, since a
	// participant that cannot run it is not a home for it.
	void DispatchRpcCall(const FCrowdyInboundEvent& Event, int32 RemainingAttempts);

	/**
	 * Offers a call to the registered subscriber, for an entity with no participant and for one whose
	 * participant declares nothing that can receive the function.
	 *
	 * Returns true when the call is finished with here (delivered, or dropped because the subscriber holds
	 * the id and has nothing to deliver to), and false when the caller should go on as it always has: no
	 * subscriber, or a subscriber that does not hold the id.
	 */
	bool DispatchCallToSubscriber(const FCrowdyRpcCall& Call, UFunction* Function);

	/**
	 * Hands a call the subscriber offered no object for to whatever that subscriber registered for the
	 * function, as decoded values (see ICrowdyEntityEventHandler).
	 *
	 * This is the whole answer for an event declared on the class an entity's owner runs as a real object:
	 * the owner runs the body, and a client holding the same entity as a row applies the change the game
	 * registered for that function instead. Reached only once the call is known to be finished with here,
	 * so it never returns a verdict; a call nothing is registered for is counted and reported.
	 */
	void DispatchCallToEventHandler(const FCrowdyRpcCall& Call, UFunction* Function, const FCrowdyFnInfo& Info);

	// Counts one such drop and, the first time for a given function, says so. See
	// ReportedUnhandledEventFunctions.
	void ReportUnhandledSubscriberEvent(const UFunction* Function);

	// True when the registered subscriber has something registered to apply Function to an entity it holds
	// as data. Answered from the function alone, so it can be asked while a call is still waiting for its
	// target entity to arrive.
	bool HasSubscriberEventHandler(const UFunction* Function) const;

	// Functions already reported as having nothing registered to apply them. Keyed by the function rather
	// than by the entity: the cause is one missing registration, and a crowd means the same missing
	// registration is hit once per entity per event. Keyed by FObjectKey rather than by a raw pointer so a
	// collected function's slot cannot be recycled into a report that never fires. Bounded by the number
	// of CrowdyEvent functions this build declares, never by what arrives off the wire.
	TSet<FObjectKey> ReportedUnhandledEventFunctions;

	int32 UnhandledSubscriberEventCount = 0;
	int32 UnhandledSubscriberEventReportCount = 0;

	/**
	 * Counts one call that had nowhere to go, and answers whether this one may be reported: always true the
	 * first time and then at most once a second.
	 *
	 * Each drop reason here is decided by the function a call names and by what this client holds for the
	 * entity, neither of which changes between two calls carrying the same pair. The natural failure is
	 * therefore not one line but one line per entity per event, and every entity id involved was chosen by
	 * whoever sent the events: left unthrottled, an arbitrary sender sets the log volume. Reporting the
	 * first and throttling the rest keeps the diagnosis without letting it become the outage.
	 *
	 * The count is taken here, not at the log line, so it survives a suppressed category: a throttled line
	 * and no traffic at all look identical, and the counter is what tells them apart.
	 */
	bool CountUndeliverableCall();

	// Wall-clock instant the throttle above reopens. Not serialized, not thread-shared: the RPC receive
	// path is game thread only.
	double NextUndeliverableCallReportSeconds = 0.0;

	int32 UndeliverableCallCount = 0;
	int32 UndeliverableCallReportCount = 0;
	int32 BudgetRefusedCallCount = 0;

	// CrowdyState receive path (FCrowdyStateDelta payloads): resolve the target entity and apply
	// container, decode the changed values onto the live container via FCrowdyStateCodec, and fire each
	// changed property's parameterless CrowdyOnRep notify. For an entity WE own, applies host-precedence by
	// convention: a foreign non-host delta is dropped before decode, and a HostSourced correction is applied
	// and then adopted into the owner's shadow (AdoptHostValues) so the owner does not revert it. Defers to
	// the shared spawn-wait retry when the entity has not registered yet.
	void DispatchStateDelta(const FCrowdyInboundEvent& Event, int32 RemainingAttempts);

	/**
	 * Reads the layout indices a delta's blob carries, from its selector alone, without decoding a value.
	 *
	 * The apply path above needs this because it decodes into a buffer shared by every entity of the class,
	 * where "the value differs from what is already in the buffer" says nothing about this entity: the
	 * previous entity's delta is what is in there. What the delta carried is the only honest statement of
	 * what was delivered, and only the selector holds it.
	 *
	 * Returns false (Warning-logged) on any malformed or unrecognised selector, including a blob version
	 * this build does not know. That version covers the selector framing, so a format change makes this
	 * refuse rather than misread, and the delta drops instead of being applied against a guessed set.
	 */
	static bool ReadStateDeltaPresentIndices(const FCrowdyRepLayout& Layout, const TArray<uint8>& Blob,
		TArray<int32>& OutPresent);

	// Resolves the replicator the apply path adopts host corrections into: the injected one in tests, else the
	// world's UCrowdyStateReplicator. Null when there is no world (headless) and none was injected.
	UCrowdyStateReplicator* ResolveStateReplicator() const;

	// Queues Event for another attempt once its entity registers. DeferBudgetSeconds is a wall-clock
	// deadline measured from the event's arrival; pass zero to leave the retry count as the only limit,
	// which is what the paths that wait on an ordinary spawn do.
	void DeferEvent(const FCrowdyInboundEvent& Event, int32 RemainingAttempts, double DeferBudgetSeconds = 0.0);
	void RetryDeferredEvents();

	bool LoadConfig() const;
};
