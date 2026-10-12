#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/EngineBaseTypes.h"
#include "Mass/EntityHandle.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/Function.h"
#include "CrowdyMassGateHarness.generated.h"

class UCrowdyEventRouter;
class UCrowdyMassAnimSet;
class UCrowdyMassEntitySubsystem;
class UCrowdyMassGateHarness;
struct FCrowdyMassStateDeltaByteField;
struct FCrowdyMassViewPoseFragment;
struct FCrowdyMassGateWireState;

/**
 * Destroys the animation-track gate's churn entities from a fixed position inside the frame.
 *
 * The renderer hands out an animation track when animation data is batched for an entity, and frees
 * it only when the entity is put on its removal queue. Those two things happen at opposite ends of
 * one frame: batching runs in the pre-physics tick group, and the pass that drains removals and then
 * re-adds anything still batched runs at the end of the post-physics tick group. An entity destroyed
 * between those two points is removed and immediately added back as a fresh instance with a new
 * track keyed to a handle that no longer exists, and nothing can ever release that track. Destroying
 * outside that span releases cleanly whichever way the release path is written, so a gate that
 * destroys outside it measures nothing about the release path at all.
 *
 * A tick function is used rather than a task dispatched to the game thread because a dispatched task
 * is pumped at whichever tick-group boundary comes next, so the same run could land inside the span
 * one time and outside it the next. A tick group is a position, not a hope.
 */
USTRUCT()
struct FCrowdyMassGateChurnDestroyTick : public FTickFunction
{
	GENERATED_BODY()

	// Cleared when the harness unregisters this, which it does before it can be destroyed. Not a
	// UPROPERTY for the same reason the engine's own actor and component tick functions hold their
	// targets raw: the tick function is owned by the thing it points at.
	UCrowdyMassGateHarness* Harness = nullptr;

	virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread,
		const FGraphEventRef& MyCompletionGraphEvent) override;
	virtual FString DiagnosticMessage() override;
};

template<>
struct TStructOpsTypeTraits<FCrowdyMassGateChurnDestroyTick> : public TStructOpsTypeTraitsBase2<FCrowdyMassGateChurnDestroyTick>
{
	enum
	{
		WithCopy = false
	};
};

/** One gate's verdict, held until the run prints its report. */
USTRUCT()
struct FCrowdyMassGateResult
{
	GENERATED_BODY()

	FString Name;

	// What was actually observed, in the units the gate measures. Recorded whether the gate passed or
	// failed: a passing gate that says what it saw is the only way to notice it is measuring the wrong
	// thing, and a failure that names no number cannot be acted on.
	FString Observed;

	bool bPassed = false;

	// Set when the gate could not be attempted at all, which is neither a pass nor a failure. Reported
	// separately for the same reason a build that ran no actions is neither: treating "not run" as
	// "passed" is how a suite comes to prove nothing.
	bool bSkipped = false;
};

/**
 * One gate a consuming game contributes, for a property this module cannot measure because it cannot
 * see the game's own types.
 *
 * Synchronous on purpose: a contributed gate is a single-frame measurement (a byte count, a value read
 * off a struct this module does not know), not something to watch happen over several frames, so it
 * needs none of the step machine below. Return whether the gate passed and write what was actually
 * observed, in the units the gate measures, into OutObserved; that text is printed for a pass as well
 * as for a failure. World is the world the run is happening in.
 *
 * A probe must not spawn, destroy, or send anything: a run's later gates read state a probe would
 * disturb.
 */
using FCrowdyMassGateProbe = TFunction<bool(UWorld* World, FString& OutObserved)>;

/** Where a run currently is. Each step either completes or times out; none of them blocks. */
UENUM()
enum class ECrowdyMassGateStep : uint8
{
	Idle,
	Spawn,
	FireOwned,
	ObserveWrite,
	ObserveApplied,
	ObserveRevert,
	DeferInject,
	DeferSpawn,
	StaleInject,
	StaleObserve,
	AuthorityRefuse,
	EchoSpawn,
	EchoPoseApply,
	EchoPoseChange,
	EchoPoseSet,
	EchoPoseCleared,
	EchoUnmakePush,
	EchoUnmakeAssert,
	EchoLegacy,
	EchoForged,
	ChurnBaseline,
	ChurnSettle,
	ChurnDestroy,
	ChurnDrain,
	PoseBankSlots,
	StateSpawn,
	StateDeliver,
	StateGenericField,
	ExtraGates,
	Report
};

/**
 * Every counter the CrowdyState gates compare, read at one instant.
 *
 * These counters are per world and count every entity, not just the one under test, so each gate is a
 * DIFFERENCE across the window it sent its deltas in rather than a total.
 *
 * The gates then assert EXACT equality on those differences, and that is only sound because the window
 * contains no yield point: handing a delta to the router runs the claim, the class guard, the decode
 * and the fragment write as one synchronous call chain, and inbound network traffic is drained once
 * per frame on the same thread. So nothing but this harness's own dispatches can move a counter
 * between the two readings, whatever else is alive in the session. If that ever stops being true, if a
 * delta is ever applied from a task or a later frame, these gates would start reporting other traffic
 * as their own and the equality would have to become a bound.
 */
struct FCrowdyMassGateStateCounters
{
	int64 Claimed = 0;
	int64 Applied = 0;
	int64 DeliveredSlots = 0;
	int64 NotClaimed = 0;
	int64 DroppedNoEntityClass = 0;
	int64 DroppedClassIdMismatch = 0;
	int64 DroppedNoLayout = 0;
	int64 DroppedNoScratch = 0;
	int64 DroppedDecodeFailed = 0;
	int64 PoseWritesFromState = 0;
};

/**
 * Drives the one-shot action path and checks it, end to end, from one command.
 *
 * It exists because the phase that built one-shot actions shipped a receive path with no way to send
 * into it: nothing outside the automation tests called either event, so every acceptance check needed
 * whoever was running it to invent a trigger first. This is that trigger, and then the checks, so a
 * run is one command and one block of output rather than a session of manual steps whose result is
 * whatever the operator remembers.
 *
 * What it covers, and what it cannot. It drives the real send path with `crowdy.rpc.loopback` on, so a
 * fragment write here proves serialize, route, resolve-the-avatar and dispatch all ran; and it feeds
 * the router directly to exercise the deferral queue, which is otherwise unreachable on purpose. It
 * also pushes view-state echoes through the real encode and decode functions and reads the fragments
 * they land on, and it spawns a batch of entities and destroys them from a fixed position inside the
 * frame to see whether the renderer's animation track count comes back to where it started. It runs in
 * ONE process, so the genuinely two-client properties (that the other machine runs the body exactly
 * once on the same entity, and that a pose an echo carries is actually drawn there) remain a manual
 * check, and the report says so on every run rather than reading as a complete answer.
 *
 * Most of the echo gates measure a pose byte, which THIS HARNESS registers an ingestor for through the
 * same public call a consuming game uses. That changed in Phase A.4: the module's own standard pose field
 * on this plane was deleted, because a byte there is a row id the sender chose and the pose now travels as
 * the game's own enum value on the CrowdyState plane. So these gates now measure the echo MECHANISM
 * exercised through its public surface, which is what that mechanism is for, and they prove registration,
 * encode, decode and ingestion work end to end for any field registered the same way. Whether a particular
 * game's own field lands on that game's own fragment is still that game's gate to write, not this one's.
 *
 * One echo gate is deliberately about no field in particular. It sends hostile values in every byte the
 * wire struct carries, including the names this module claims no ingestor for and a consuming game may,
 * and then waits out a frame before asserting that nothing structural moved: the entity still exists,
 * the tracker still resolves its id to the same handle, the trace grid still returns it, and its trace
 * collision bytes are the ones it had. View state is client-authoritative and forgeable, so no value in
 * it, whatever a game decides that value means, may decide whether an entity exists or can be hit.
 *
 * A separate set of gates drives the OTHER road a pose can arrive on. An echo rides along with a
 * position update; a CrowdyState delta is its own payload, decoded against the entity's class and
 * delivered to an entity that owns no object of its own. Both roads end at the same byte in the same
 * fragment, so a pose that looks right says nothing about which one carried it, and only the counters
 * kept per road can. Those gates synthesize a delta, hand it to the router the way one arriving from
 * another machine is handed over, and read the difference in both roads' counters across the window
 * they sent it in.
 *
 * Those gates spawn their entity from this plugin's OWN template rather than from the Mass entity
 * config a project configured. Every other gate here spawns from that config, which in a real project
 * is a game's asset carrying a game's traits, meshes and animation sets, so those gates are game-free
 * by where they live rather than by what they touch. These ones are game-free by construction, which
 * is the only kind of game-free a claim about this plugin can rest on.
 *
 * The last of them registers a field this module knows nothing about, through the same public call a
 * consuming game would use, and checks that the bytes reach the ingestor that registered for it. That
 * is the difference between a registration surface that exists and one that works.
 *
 * A consuming game adds gates of its own through RegisterExtraGate, for properties that depend on its
 * own types.
 *
 * Not created in shipping builds.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassGateHarness : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	static UCrowdyMassGateHarness* Get(UWorld* World);

	/**
	 * Fires one action at a live Mass entity: one along the camera's aim if the spatial grid offers a
	 * candidate there, otherwise any tracked entity. The manual counterpart to the automated run, for
	 * looking at a real crowd rather than at a verdict.
	 */
	void FireOneShot(int32 ActionId, int32 Variant, float PlayRate);

	/**
	 * Sends the SDK's generic Crowdy action at a live Mass entity through
	 * UCrowdyMassTargetLibrary::SendEntityAction: one along the camera's aim if the spatial grid offers a
	 * candidate there, otherwise any tracked entity. The manual counterpart to a graph's Send Crowdy
	 * Action node, for driving that same path from the console with no game code in the way.
	 *
	 * This is a test caller, not a real one: it makes the action path runnable by a human and proves
	 * nothing about whether any game has adopted it.
	 */
	void SendGenericAction(FName ActionName, int32 Variant, float PlayRate);

	/**
	 * Runs every automated gate from a clean state and prints one report.
	 *
	 * ChurnEntityCount is how many entities the animation-track leak gate spawns and destroys. It is
	 * clamped, because the number is a load knob rather than a correctness one: too few and the track
	 * count may never rise far enough for its return to mean anything, too many and the run stalls the
	 * session it is measuring.
	 */
	void RunGates(int32 ChurnEntityCount);

	/** Abandons a run in progress, restoring whatever it changed. */
	void StopGates();

	/**
	 * Adds a gate this module cannot write itself, because measuring it means reading a type only the
	 * consuming game has. Every registered gate runs, in registration order, in one step just before the
	 * report, and its verdict is printed alongside the built-in ones.
	 *
	 * Static, and safe to call before any world exists, because the natural place to call it is a game
	 * module's startup. Registering a name that is already present replaces its probe, so a hot reload
	 * that runs startup again leaves one gate rather than two.
	 */
	static void RegisterExtraGate(FName Name, FCrowdyMassGateProbe Probe);

	/**
	 * Removes a contributed gate. Call it from the shutdown of whichever module registered it: a probe
	 * captures that module's state and outlives the module otherwise.
	 */
	static void UnregisterExtraGate(FName Name);

	/**
	 * Destroys the churn entities and records where in the frame that happened. Called from the gate's
	 * own tick function and from nowhere else: the position in the frame is the measurement, so a
	 * caller that is not that tick function would be recording a position it did not occupy.
	 */
	void DestroyChurnEntitiesInsideFrame();

private:
	bool Tick(float DeltaSeconds);

	// Moves to the next step. Every step is written so that doing nothing for long enough is a failure
	// rather than a hang, which is why each one carries a deadline.
	void Advance(ECrowdyMassGateStep Next, float TimeoutSeconds);
	bool HasStepTimedOut() const;

	void Record(const FString& Name, bool bPassed, const FString& Observed);
	void Skip(const FString& Name, const FString& Why);

	// Spawns one entity and stamps its identity so this client is allowed to speak for it. The send path
	// refuses an entity the local client is not the authority for, which is correct and is itself gated
	// below; a run that never stamped one would only ever measure that refusal.
	bool SpawnGateEntity(int32 Index, const FGuid& UUID);
	void DestroyGateEntities();

	// Reads one gate entity's action fragment. Returns false when the entity or the fragment is gone,
	// which every caller treats as a failed observation rather than as a zero.
	bool ReadAction(int32 Index, int32& OutActionId, int32& OutSerial, int32& OutAppliedSerial) const;

	// Hands the router an event addressed at an id nothing has spawned, which is the only way to reach
	// the deferral queue deliberately.
	bool InjectInboundForUnknownEntity(const FGuid& UUID);

	// Gives the harness's own wire struct a type tag, through the same call the auto registry makes for
	// a game's executor-declared struct. False when it has none afterwards, which every view-state gate
	// treats as a missing precondition rather than as a failure: with no tag nothing can be encoded, so
	// there is no decode to measure.
	bool EnsureEchoPayloadTypeRegistered() const;

	// Encodes State the way a producer does. TruncateTrailingByteCount drops that many bytes off the
	// end, which is how a legacy payload is built: the bytes an older producer sends are exactly the
	// bytes a current one sends minus the fields it never had.
	bool EncodeEcho(const FCrowdyMassGateWireState& State, int32 TruncateTrailingByteCount, TArray<uint8>& OutBytes) const;

	// Decodes Bytes through the live decode path and lands it on the echo entity. OutDecoded is what came
	// back off the wire, so a gate can report a field's decoded value rather than the value it sent.
	// OutWhy names what went wrong, so a caller can report the reason and not only the verdict.
	bool PushEcho(const TArray<uint8>& Bytes, FInstancedStruct& OutDecoded, FString& OutWhy);

	// The echo entity's view pose fragment, or null when the entity or that fragment is gone. Re-read on
	// every use rather than cached: a fragment pointer is into Mass storage and does not survive the
	// entity changing archetype.
	FCrowdyMassViewPoseFragment* GetEchoPose() const;

	// The echo entity's trace collision fragment, copied out as raw bytes. False when the entity carries
	// no such fragment, which is a missing precondition for the gate that compares two of these rather
	// than a difference between them.
	bool CopyEchoTraceCollisionBytes(TArray<uint8>& OutBytes) const;

	// Marks every view-state echo gate as not attempted, naming the one precondition that stopped all of
	// them. Used where the shared setup fails, so the report says which gates did not run instead of
	// falling silent about them.
	void SkipEveryEchoGate(const FString& Why);

	// Writes a pose id no gate ever sends onto the echo entity. Without it a gate asserting a decoded
	// value would be satisfied by an entity nothing had ever written to, and would keep passing with the
	// decode path removed.
	void PoisonEchoPose();

	// Spawns the entity the CrowdyState gates drive, from this plugin's own template rather than from
	// whatever Mass entity config the project configured, and records the class those deltas will claim
	// to be. OutWhy names the precondition that stopped it, which those gates report as not attempted.
	bool SpawnStateEntity(FString& OutWhy);

	// The state entity's view pose fragment, or null when the entity or that fragment is gone. Re-read on
	// every use rather than cached, for the same reason the echo entity's is.
	FCrowdyMassViewPoseFragment* GetStatePose() const;

	// Writes the same unsent pose id onto the state entity that the echo gates write onto theirs. Both
	// roads end at this byte, so an assertion about either has to start from a value neither could have
	// produced.
	void PoisonStatePose();

	// Reads every counter the CrowdyState gates compare. False when the router or the entity subsystem is
	// gone, which those gates report as not attempted rather than as a reading of zero.
	bool ReadStateCounters(FCrowdyMassGateStateCounters& OutCounters) const;

	// Builds one CrowdyState delta carrying the named byte fields and hands it to the router the way one
	// arriving from another machine is handed over. The class it encodes as is asked of the same source
	// the receive path's own guard reads, so a delta this builds and the entity it is addressed to cannot
	// disagree about the class. OutWhy names what stopped it.
	bool DispatchStateDelta(TConstArrayView<FCrowdyMassStateDeltaByteField> Fields, FString& OutWhy);

	// Marks every CrowdyState gate as not attempted, naming the one precondition that stopped all of
	// them, for the same reason SkipEveryEchoGate exists.
	void SkipEveryStateGate(const FString& Why);

	// Animation tracks and live instances, summed over every instanced-skinned-mesh shared data in this
	// world. False when this world has no such shared data at all, which is not zero tracks: it means the
	// crowd is not drawn through that path here and the leak gate has nothing to measure.
	bool ReadRendererTrackCounts(int32& OutTrackCount, int32& OutLiveInstanceCount) const;

	void DestroyChurnEntities();

	// Registers and enables the churn destroy tick function against this world. False when there is no
	// level to register it on, which the gate reports as not attempted rather than destroying the
	// entities from somewhere else: a destroy outside the frame span would pass whether or not the
	// release path is correct.
	bool ArmChurnDestroyTick();
	void DisarmChurnDestroyTick();

	// Where in the frame the destroy actually happened, in words, for the gate's verdict. Every L1
	// verdict and skip carries it, because a reading taken at the wrong point in the frame is not a
	// weaker result than one taken at the right point, it is a different measurement.
	FString DescribeChurnDestroyPosition() const;

	// Reads the entity's anim set and the bank it renders through, resolves the two against each other,
	// and reports whether every authored pose came out with a bank index of its own. OutObserved is
	// filled either way. bOutSkipped marks the outcomes that are an authoring gap rather than a defect.
	bool ValidatePoseBankSlots(FString& OutObserved, bool& bOutSkipped) const;

	UCrowdyMassEntitySubsystem* GetMassSubsystem() const;
	UCrowdyEventRouter* GetRouter() const;

	void PrintReport();

	FTSTicker::FDelegateHandle TickHandle;

	// The world the run started in. This subsystem belongs to the game instance and its ticker is the core
	// ticker, so both outlive a level change, while everything a run measures belongs to one world: the
	// entity subsystem and the router are resolved through whatever world is current, and a Mass entity
	// handle is an index and a serial number issued by one world's entity manager, with serial numbers
	// starting again from zero in the next one. So the handles a run is carrying stay VALID across a
	// travel and start naming unrelated entities, which is worse than them going stale. A run that
	// noticed nothing would write its test values onto live entities in a level it never measured.
	TWeakObjectPtr<UWorld> RunWorld;

	ECrowdyMassGateStep Step = ECrowdyMassGateStep::Idle;
	double StepStartedSeconds = 0.0;
	float StepTimeoutSeconds = 0.0f;

	TArray<FCrowdyMassGateResult> Results;

	// The entities a run uses: index 1 is the one fired at, 0 and 2 are its neighbours and exist to
	// prove an action lands on one entity rather than on the archetype.
	TArray<FGuid> EntityUUIDs;
	TArray<FMassEntityHandle> EntityHandles;

	// Ids that must NOT exist when the deferral gates run.
	FGuid DeferUUID;
	FGuid StaleUUID;

	// Readings taken when a gate fires, compared against later ones.
	int32 SerialAtFire = 0;
	int32 DeferredAtInject = 0;
	int32 EvictionsAtInject = 0;
	bool bActionWasApplied = false;

	// The loopback console variable's value before the run, restored afterwards. A run that left it on
	// would change how every later send in the session behaves.
	int32 LoopbackBeforeRun = 0;
	bool bChangedLoopback = false;

	// The entity the view-state echoes are pushed at, kept apart from the action entities above so a
	// pose write can never be mistaken for something the action gates did.
	FGuid EchoUUID;
	FMassEntityHandle EchoEntity;
	FVector EchoLocation = FVector::ZeroVector;

	// The echo entity's trace collision fragment as its raw bytes, taken immediately before a pose echo
	// is sent. Held as bytes because the comparison this feeds is byte-for-byte: the gate's claim is that
	// a pose touches none of it, and a field-by-field check would only cover the fields somebody
	// remembered.
	TArray<uint8> EchoTraceCollisionBefore;

	// The pose id actually observed on the fragment after the first pose echo, which the second echo's
	// reading is compared against. Comparing against a constant instead would pass on an entity whose
	// pose never moved from one it happened to be sitting on already.
	uint8 EchoPoseAfterApply = 0;

	// The frame the hostile echo was pushed on. The gate that reads it asserts only once the counter has
	// moved past this, because a game's processors and its Blueprint listeners do not run until the next
	// processing phase: an assertion taken in the same tick as the push cannot see one of them destroy
	// the entity, which is the whole failure this gate exists to catch.
	uint64 EchoUnmakeFrameCounter = 0;

	// Whether that echo was shown to have reached the entity. False turns the gate into a not-attempted
	// report rather than a pass: readings taken over an echo that never landed are the readings an entity
	// nobody echoed at would give.
	bool bEchoUnmakeIngested = false;

	// What the spatial grid said about the entity immediately BEFORE that echo. A grid that cannot answer
	// hands back every tracked entity, and a grid that never held the entity offers no "still" to observe;
	// both are recorded so the gate can report that leg as not attempted rather than judging it.
	bool bEchoUnmakeGridAnswered = false;
	bool bEchoUnmakeInGridBefore = false;

	// The entity's trace collision fragment as raw bytes immediately before that echo. Kept separately
	// from the reading the pose gates take, because these two are compared across different echoes and
	// sharing one buffer would let either gate report the other's difference.
	TArray<uint8> EchoUnmakeCollisionBefore;

	// The entity the CrowdyState deltas are driven at, kept apart from both the action entities and the
	// echo entity so a pose write on this one can only have come from the road under test.
	FGuid StateUUID;
	FMassEntityHandle StateEntity;

	// The animation set the gate class poses through, authored by the harness itself so the gates assert
	// rows whose ORDER they chose. Held as a UPROPERTY because the subsystem's binding is what the run
	// depends on and nothing else references this object: collected mid-run, the gate class would silently
	// go back to naming no set and every pose gate would read as the receive path being broken.
	UPROPERTY(Transient)
	TObjectPtr<UCrowdyMassAnimSet> GateAnimSetKeepAlive;

	// The peer those deltas claim to come from. It has to be a valid id that is not this client's own,
	// because a delta whose sender is us is dropped before anything else runs: a rig that merely re-sent
	// its own bytes would report success while testing the guard rather than the feature.
	FGuid StateSenderID;

	// How many CrowdyState properties the state entity's class carries, resolved once from the same
	// registry the receive path resolves it from. The delivered-slot arithmetic is a multiple of it, so a
	// verdict that quoted a guessed number would pass or fail for the wrong reason.
	int32 StateLayoutSlotCount = 0;

	// Both roads' counters immediately before the deltas are dispatched. Every CrowdyState verdict is the
	// difference against a reading taken after, never a total.
	FCrowdyMassGateStateCounters StateCountersBefore;

	// What the ingestor registered from outside this module's own standard set was handed, recorded by the
	// ingestor itself. Read back rather than inferred from a fragment, because the whole question that gate
	// asks is whether the registration reached the ingestor at all, and a fragment another field wrote
	// would answer it wrongly.
	bool bGateFieldIngestorRan = false;
	uint8 GateFieldValueSeen = 0;

	// Whether that ingestor could resolve a fragment through the context it was handed. A game's own
	// ingestor exists to write one, so an ingestor that runs but cannot reach a fragment has a working
	// registration and no way to use it.
	bool bGateFieldReachedAFragment = false;

	// The entities the animation-track gate spawns, and the readings it compares.
	TArray<FGuid> ChurnUUIDs;
	int32 ChurnEntityCountRequested = 0;
	int32 ChurnEntitiesSpawned = 0;
	int32 TracksAtBaseline = 0;
	int32 InstancesAtBaseline = 0;

	// The highest track count seen while the churn entities were alive, not the count at the end of that
	// window. A count read only at the end cannot tell "the tracks came and went within the window" from
	// "the tracks were never handed out at all", and those two need opposite verdicts.
	int32 TracksAtPeak = 0;
	int32 InstancesAtPeak = 0;

	// Destroys the churn entities from a fixed position inside the frame. See the type's own comment
	// for why the position is the point of the gate rather than an implementation detail.
	FCrowdyMassGateChurnDestroyTick ChurnDestroyTick;

	// What that tick function recorded when it ran: whether it ran at all, the tick group the world
	// reported itself to be executing at that moment, and the frame it happened on. Read back rather
	// than assumed, so a run that was demoted to a later group says so instead of claiming the group it
	// asked for.
	bool bChurnDestroyRan = false;
	TEnumAsByte<ETickingGroup> ChurnDestroyTickGroup = TG_MAX;
	uint64 ChurnDestroyFrameCounter = 0;
};
