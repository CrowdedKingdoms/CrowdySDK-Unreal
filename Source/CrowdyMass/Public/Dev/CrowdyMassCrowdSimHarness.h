#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Core/FCrowdyTypeID.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyMassCrowdSimHarness.generated.h"

class UCrowdySDKSubsystem;
class UEnum;
struct FCrowdyRepLayout;
class FIntProperty;
class FNumericProperty;
class FProperty;

/**
 * What a simulated player is doing right now. The names are movement intents, not animation gaits:
 * this harness deliberately knows nothing about which gait a given speed selects, so that a change
 * to the authored gait thresholds cannot quietly change what the load test is testing.
 */
UENUM()
enum class ECrowdyMassCrowdSimPhase : uint8
{
	Standing,
	Walking,
	Jogging,
	Running,
	// Holds a speed pinned to one of the configured thresholds. This is the case gait hysteresis
	// exists for, and the one a human at a keyboard almost never produces on purpose: a player
	// travelling at exactly the speed at which one gait becomes another. At crowd scale a selector
	// with no hysteresis shows it as a field of flickering animation.
	HoldingAtThreshold
};

/** One simulated player. Plain data, walked as an array once per emission tick. */
USTRUCT()
struct FCrowdyMassCrowdSimBot
{
	GENERATED_BODY()

	// 32 hex characters, the form a real entity's id takes on the wire. Derived from the run seed and
	// the bot's index, so the same bot has the same id on every run and two runs can be compared.
	FString ActorId;

	FVector Location = FVector::ZeroVector;

	// Radians. Yaw only; simulated players do not pitch or roll.
	float HeadingRadians = 0.0f;

	float SpeedCentimetresPerSecond = 0.0f;

	// Where the bot is heading towards over the remainder of this phase. Speed eases towards it so a
	// bot accelerates and decelerates rather than teleporting between speeds, which is what makes it
	// pass through the intermediate gaits on the way.
	float TargetSpeedCentimetresPerSecond = 0.0f;

	float TurnRateRadiansPerSecond = 0.0f;

	ECrowdyMassCrowdSimPhase Phase = ECrowdyMassCrowdSimPhase::Standing;

	float SecondsUntilNextPhase = 0.0f;

	// Per-bot stream, seeded from the bot's index, so a bot's whole path is reproducible and adding
	// or removing bots does not change what the others do.
	FRandomStream Random;

	// The CrowdyState id: the actor id converted, or a fresh guid under local delivery so participant resolution cannot find the local entity.
	FGuid StateEntityID;

	// The lightweight CrowdyState participant spawned for this bot at Start, as whatever class the run
	// was started with. Null until then, and again once DestroyStateEntities runs it back to null at Stop;
	// EmitBotState leaves a bot with no live entity alone rather than treating a spawn failure as fatal to
	// the whole run.
	TWeakObjectPtr<AActor> StateEntity;

	// Counts down, in emission-interval units, to this bot's next CrowdyState payload change. See EmitBotState.
	float SecondsUntilNextStateChange = 0.0f;
};

// A bot class's int32 Payload and enum-shaped Stance, found by name and type alone since cooked classes carry no metadata; a missing field is null.
struct FCrowdyMassCrowdSimBotFields
{
	const FIntProperty* PayloadProperty = nullptr;
	const FProperty* StanceProperty = nullptr;
	const UEnum* StanceEnum = nullptr;

	// Set when Stance is an FEnumProperty, whose value is written through this underlying property; a byte Stance leaves it null.
	const FNumericProperty* StanceUnderlyingProperty = nullptr;

	// StanceEnum's GetMaxEnumValue, read once so the per-bot path never touches the UEnum.
	int64 StanceMaxValue = 0;
};

/**
 * Dev-only traffic generator: N simulated players emitting position updates at a fixed rate through
 * the real SDK send path, so the receiving client can be measured under a crowd it would otherwise
 * need N machines to produce.
 *
 * It is deliberately hosted in the engine rather than written as a standalone sender. A hand-rolled
 * sender would be a second implementation of the wire format, of the chunk maths and of the session
 * bootstrap, and two implementations agreeing proves nothing about the one a game actually ships.
 * Every update here goes out through UCrowdySDKSubsystem::DispatchActorUpdate, the same call the
 * automatic replicator makes for a real player, with the chunk computed by the same helper.
 *
 * Run it in a headless instance (-nullrhi -unattended) and observe from a normal client. The
 * measurements this exists to feed are taken on the OBSERVER, not here: frames, entity and animation
 * track counts, and whether gaits look right. What this side reports is what it emitted and how the
 * bots are distributed, which is what turns an observer reading into a number per relevant entity.
 *
 * This generator owns no sign-in of its own. It only checks that a Crowdy SDK subsystem exists; whatever
 * session that subsystem is carrying, signed in or not, is what every update goes out under. A headless
 * process driving -crowdsim has to establish its own session before this runs, by whatever means the
 * project's own start-up already uses (a saved session, a prior login step), or updates will be built
 * and handed to a subsystem with nothing to send them as.
 *
 * Not created in shipping builds.
 */
UCLASS()
class CROWDYMASS_API UCrowdyMassCrowdSimHarness : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	static UCrowdyMassCrowdSimHarness* Get(const UWorld* World);

	/**
	 * Starts (or restarts) the generator. RadiusMetres is the radius of the disc the bots wander within,
	 * centred on the local player if there is one; zero or negative falls back to the default. BotClass
	 * null means the plugin's own ACrowdyMassCrowdSimStateEntity fixture; anything else must be an Actor
	 * subclass whose default object carries a UCrowdyEntityComponent, or the call is refused. Returns
	 * false and logs why when it refuses.
	 */
	bool Start(int32 BotCount, float RadiusMetres, UClass* BotClass);
	void Stop();

	bool IsRunning() const { return bRunning; }

	// Bots currently simulated, 0 when stopped.
	int32 GetBotCount() const { return Bots.Num(); }

	// The class the running (or last) simulation spawned, null before the first start.
	UClass* GetBotClass() const { return BotClass; }

	// What is being emitted, and how the bots are spread over the world. The spread is the half of
	// the load test the observer cannot see: a thousand bots inside one chunk and a thousand spread
	// over a hundred are the same entity count and completely different network conditions.
	void Status();

	/**
	 * Keep the simulated crowd's updates inside this process instead of sending them, delayed by the
	 * given one-way latency, so that this machine can both generate and observe a crowd far larger than
	 * its uplink could carry.
	 *
	 * The reason this exists is a measurement, not a preference. An actor update is 185 octets on the
	 * wire with no client-to-server bundling, so two thousand players is about thirty megabits a second
	 * upstream, and a saturated uplink presents from inside the run as jitter and teleporting that look
	 * exactly like an SDK fault. Local delivery removes the link from the question.
	 *
	 * What it therefore measures is what this client costs to RECEIVE a crowd of a given size: the
	 * receive drain, the tracker, the entity and animation work, and the frame. What it cannot measure
	 * is anything the server decides, above all which updates the relay would have judged relevant to
	 * this observer, so a chunk-occupancy or relevance question still needs a real server.
	 *
	 * Latency and jitter are what a real update spends between two machines. They are not decoration:
	 * an update delivered in the call that sent it would give interpolation and arrival-interval
	 * estimation a cadence no network produces.
	 *
	 * Takes effect at the next Start, or immediately when a run is already going. Any negative float
	 * argument leaves that setting unchanged.
	 */
	void SetLocalDelivery(bool bEnable, float LatencyMs, float JitterMs, float LossPercent);

	bool IsLocalDeliveryArmed() const { return bLocalDelivery; }

	/**
	 * How often, on average in seconds, one simulated player's CrowdyState payload changes. Deliberately
	 * separate from both the 10 Hz transform cadence and the keyframe heartbeat (a map-profile setting this
	 * harness does not own): the standing cost a crowd's worth of CrowdyState puts on the wire is dominated
	 * by the heartbeat, not by on-change traffic, and a generator that only ever produces one of the two
	 * would measure the wrong thing. Zero or negative turns on-change traffic off entirely; the heartbeat
	 * still fires on its own schedule regardless. Takes effect as each bot's own timer next comes due, same
	 * as SetLocalDelivery's live-update semantics.
	 */
	void SetStateChangeInterval(float Seconds);

	// The interval in force, so the console command can report the setting without choosing a new one.
	float GetStateChangeInterval() const { return StateChangeIntervalSeconds; }

	/**
	 * Runs the whole receive-cost profiling session unattended: three passes, each configuring its own
	 * trace scopes, settling, tracing for the given window, reporting, and stopping, into three separate
	 * .utrace files.
	 *
	 * Three files rather than one because the extra decode scopes nest inside scopes that are themselves
	 * measured, so a run that carries one is not comparable with a run that does not. Pass A carries none
	 * and is the only one comparable against an earlier baseline; B sizes the actor-state struct walk; C
	 * sizes the CrowdyState delta decode and raises the state-change rate so those deltas are a real share
	 * of the mix rather than a rounding error.
	 *
	 * Each pass settles before tracing starts, so the ramp and the first long frame after a level load
	 * stay out of the sample. Returns false and changes nothing if a session is already running.
	 * BotClass is what every pass spawns, null meaning the plugin's own fixture, as for Start.
	 */
	bool StartProfile(int32 BotCount, float TracedSecondsPerPass, float LastPassStateInterval, UClass* BotClass);

	// Whether an unattended profiling session is in flight, so a second request is refused rather than
	// interleaved.
	bool IsProfileSessionRunning() const { return ProfileStep != EProfileStep::Idle; }

	// The one place reflection is read for a bot class; needs no world, so a test can call it on a bare class.
	static FCrowdyMassCrowdSimBotFields ResolveBotFields(const UClass* BotClass);

private:
	// Reads the command line and, if a run was asked for, starts it once the first map has loaded: at Initialize no map or session exists yet.
	void StartFromCommandLineIfAsked();
	void StartFromCommandLineOnMapLoaded(UWorld* LoadedWorld);

	int32 PendingCommandLineBotCount = 0;
	float PendingCommandLineRadiusMetres = 0.0f;
	UPROPERTY()
	TObjectPtr<UClass> PendingCommandLineBotClass;
	FDelegateHandle PostLoadMapHandle;

	bool Tick(float DeltaSeconds);

	void SeedBots(int32 BotCount, float RadiusMetres);
	// Centre is passed in rather than looked up here: this runs once per bot per frame, and resolving
	// the local player inside it would repeat that lookup thousands of times for a value that cannot
	// change within a frame.
	void AdvanceBot(FCrowdyMassCrowdSimBot& Bot, const FVector& Centre, float DeltaSeconds) const;
	void ChooseNextPhase(FCrowdyMassCrowdSimBot& Bot) const;
	void EmitBot(FCrowdyMassCrowdSimBot& Bot, UCrowdySDKSubsystem& SDK);

	// Emits a contiguous run of bots. The run is contiguous because each bot's slot in the interval
	// is its index, which keeps the emitted set a plain slice of the array rather than a search.
	void EmitRange(int32 FirstIndex, int32 LastIndex, UCrowdySDKSubsystem& SDK);

	// The CrowdyState half of a bot's emission, called from EmitBot at the same staggered per-bot cadence
	// as the transform update beside it. Keeps the bot's CrowdyState entity's location in step with its
	// simulated position, then, no more often than once every StateChangeIntervalSeconds (redrawn per bot
	// so a crowd's worth of bots do not land on the same tick), writes new values into whichever of the
	// bot class's Payload and Stance fields resolved so the real replicator's own diff picks them up and
	// puts a delta on the wire on its own schedule. A no-op for a bot whose entity failed to spawn or has
	// since been destroyed.
	void EmitBotState(FCrowdyMassCrowdSimBot& Bot);

	/**
	 * Hands this bot's stance value straight to the receive path as though a peer had sent it, for a
	 * local run.
	 *
	 * Only meaningful with local delivery on, and it exists because CrowdyState cannot be looped back the
	 * way the transform channel can. Three correct guards stand between a delta this process emitted and
	 * this process applying it (own-sender drop, foreign-non-host drop for an entity we own, and
	 * participant resolution finding the locally spawned entity first), so a loopback that only re-sent
	 * the bytes would report success while testing nothing.
	 */
	void DeliverStateLocally(const FCrowdyMassCrowdSimBot& Bot, int64 StanceValue);

	// Writes the resolved Stance field on Container through the facts ResolveBotFields cached, so no cast runs per bot.
	static void WriteStanceValue(const FCrowdyMassCrowdSimBotFields& Fields, void* Container, int64 Value);

	// Writes StanceValue into whichever of Layout's properties carries the bot class's stance enum; nothing when Stance did not resolve.
	void WriteStanceIntoTemplate(const FCrowdyRepLayout& Layout, UObject* Template, int64 StanceValue) const;

	// Property containers standing in for the peer this process pretends to be, one per class a crowd row was
	// activated as. UPROPERTY so they survive collection for the life of the run.
	UPROPERTY()
	TMap<TObjectPtr<const UClass>, TObjectPtr<UObject>> LocalDeliveryTemplates;

	// The peer identity a locally delivered delta claims to come from. Anything but this client's own id,
	// which is the first thing the receive path drops on, and stable for the run so the receive side sees one
	// peer rather than a new one per delta.
	FGuid LocalDeliverySenderID;

	// Spawns one hidden, non-ticking BotClass participant per seeded bot under its StateEntityID; a failed spawn leaves that bot with no CrowdyState traffic.
	void SpawnStateEntities();

	// Destroys every bot's CrowdyState entity and clears the weak pointer to it. Called from Stop: unlike
	// the transform channel, where a stopped run's simulated players are deliberately left for the tracker's
	// own timeout, a CrowdyState entity is a real UObject this process owns and must tear down itself.
	void DestroyStateEntities();

	// Converted from the bot's own actor id through the SDK's conversion, so both of a bot's channels name
	// the SAME entity on the observer. An independently mixed id would be a well-formed id for an entity the
	// observer has never heard of, and its state deltas would resolve to nothing.
	static FGuid MakeDeterministicStateEntityId(const FString& ActorId);

	// Resolved once, at Start. The local player if there is one, otherwise the world origin.
	FVector ResolveCrowdCentre() const;

	// Hands the SDK the ids it should keep local. Called after the bots are seeded, since the ids do
	// not exist before that; a no-op when local delivery is off.
	void ApplyLocalDelivery();

	// Gives the SDK back the ids, so a stopped run does not leave later traffic being intercepted.
	void ClearLocalDelivery();

	// What local delivery did over the interval just measured, and what its queue depth says about
	// whether this client can keep up with the crowd it is being shown.
	void ReportLocalDelivery(double ReportSeconds);

	// 32 hex characters derived from the run seed and the bot index. The wire form of an actor id is
	// 32 opaque octets, hex by convention, which is what a real entity's id serialises to.
	static FString MakeDeterministicActorId(int32 RunSeed, int32 BotIndex);

	// Where an unattended profiling session has got to. Its own ticker rather than the generator's,
	// because the session has to keep running through the gaps where the generator is stopped.
	enum class EProfileStep : uint8
	{
		Idle,
		Settle,
		Tracing,
		Cooldown,
	};

	bool ProfileTick(float DeltaSeconds);
	void BeginProfilePass();
	void FinishProfileSession();

	// Applies one pass's scope CVars and, for the last pass, its raised state-change rate.
	void ApplyProfilePass(int32 PassIndex);

	// Restores whatever the scope CVars and the state-change interval were before the session.
	void RestoreProfileSettings();

	FTSTicker::FDelegateHandle ProfileTickHandle;
	EProfileStep ProfileStep = EProfileStep::Idle;
	int32 ProfilePassIndex = 0;
	int32 ProfileBotCount = 0;
	UPROPERTY()
	TObjectPtr<UClass> ProfileBotClass;
	float ProfileTraceSeconds = 0.0f;
	float ProfileStateInterval = 0.0f;
	double ProfileStepDeadlineSeconds = 0.0;
	FString ProfileSessionStamp;
	TArray<FString> ProfileTraceFiles;

	// Captured at the start of the session and put back at the end, so a session cannot leave a scope
	// switched on and silently distort whatever is measured next.
	int32 ProfileSavedSerializeScopes = 0;
	int32 ProfileSavedStateScopes = 0;
	float ProfileSavedStateInterval = 0.0f;

	UPROPERTY()
	TArray<FCrowdyMassCrowdSimBot> Bots;

	FTSTicker::FDelegateHandle TickHandle;

	/**
	 * How far through the emission interval the generator currently is, wrapping at the interval.
	 *
	 * Each bot owns a slot in that interval, at its index, and is emitted as the phase sweeps past
	 * it. That spreading is not a refinement, it is the difference between traffic that resembles a
	 * crowd and traffic no crowd could produce: real clients each keep their own clock and their
	 * sends land scattered across the interval, whereas emitting the whole crowd in one frame
	 * presents the socket with the entire population at once, ten times a second. The socket refuses
	 * the overflow and the updates that do arrive are bunched, which shows up on the observer as
	 * entities jittering because their updates land late and unevenly.
	 *
	 * Wall-clock rather than per-frame for the separate reason that a headless instance runs at a
	 * wildly different frame rate to the client being measured; pacing per frame would make the
	 * traffic a property of the generator's frame rate rather than of the update rate under test.
	 */
	float EmissionPhaseSeconds = 0.0f;

	float RadiusCentimetres = 0.0f;

	// See SetStateChangeInterval. Read by SeedBots (to stagger each bot's first change) and by EmitBotState
	// (to redraw each bot's next one); changing it live takes effect as each bot's own timer next expires.
	float StateChangeIntervalSeconds = 5.0f;

	// Fixed for the whole run, deliberately. Following the local player would mean that walking the
	// observer around drags the entire crowd with it, and a bot pulled back inside the radius by that
	// move reports a speed it never travelled at. Gait selection reads exactly that speed, so a
	// moving centre would corrupt the measurement this harness exists to take.
	FVector CrowdCentre = FVector::ZeroVector;

	int32 RunSeed = 0;

	// Counted rather than assumed. An emitter that silently fails to keep its rate would otherwise
	// make the observer's numbers describe a smaller crowd than the one that was asked for.
	int64 UpdatesEmitted = 0;

	// The class every simulated player is spawned as, resolved once at Start and kept afterwards so
	// GetBotClass can report the last run's class. Null before the first Start.
	UPROPERTY()
	TObjectPtr<UClass> BotClass;

	// BotClass's Payload and Stance fields, resolved once at Start alongside BotClass.
	FCrowdyMassCrowdSimBotFields BotFields;

	// The wire class every simulated player claims on the transform channel, resolved once from BotClass.
	// Every bot is the same class, and the lookup builds the class path into a string and hashes it, which
	// at ten updates a second across ten thousand bots would be a hundred thousand allocations a second
	// inside the emit loop.
	FCrowdyClassID BotClassID = CROWDY_INVALID_CLASS_ID;

	// The same answer for the CrowdyState channel, where the class comes from the Mass subsystem per bot
	// and so cannot be assumed to be one value. Keyed like LocalDeliveryTemplates beside it, and holding
	// no reference, since it is only consulted for a class that call just returned.
	TMap<const UClass*, FCrowdyClassID> StateClassIDCache;

	// Most updates handed to the socket in a single frame. This, not the crowd size, is what a send
	// buffer has to absorb at once. Reset every report, so it describes the interval just measured
	// rather than the worst moment of the whole run, which is usually start-up.
	int32 LargestBurst = 0;

	// What the current frame has handed the socket so far, across every EmitRange call it makes. The
	// sweep calls EmitRange twice on the frame it wraps, and the socket sees both together.
	int32 BurstThisFrame = 0;

	// Frames and drops seen at the last report, so each report can describe the interval since it
	// rather than the run so far. A cumulative percentage falls steadily whatever happens next, so a
	// problem that has stopped and a problem that is ongoing look the same in it.
	int64 UpdatesAtLastReport = 0;
	int64 DropsAtLastReport = 0;

	// Deferred sends are tracked separately from drops because they are a different condition with a
	// different fix: the socket is healthy and the send buffer is simply full. Without this the report
	// would read "no sends were lost" while the buffer saturates, which is the more misleading answer.
	int64 DeferredAtLastReport = 0;

	// Bytes and datagrams at the last report, so the run can state the load it is actually offering the
	// network. A crowd emulated from one machine puts a whole population's upstream through one link,
	// and when that link is the limit every other number here describes the link rather than the SDK.
	int64 BytesAtLastReport = 0;
	int64 SentAtLastReport = 0;
	int32 FramesSinceLastReport = 0;
	double LastReportTimeSeconds = 0.0;

	double StartTimeSeconds = 0.0;

	// An unattended run has no console to ask for status from, so it reports itself on a timer.
	// Without this the only thing its log carries is the transport complaining, which says a run went
	// wrong but not what it was doing at the time.
	double NextAutoStatusTimeSeconds = 0.0;
	bool bAutoStatus = false;

	bool bRunning = false;

	// Whether the simulated crowd is delivered inside this process rather than sent. Off by default,
	// because a run that never leaves the machine cannot see anything the server decides, and that is
	// most of what a networked load test is usually asked.
	bool bLocalDelivery = false;

	// A one-way delay in the range a real session sits at, so the receiving side sees an update cadence
	// with a network's shape rather than the sender's own call stack.
	float LocalLatencyMilliseconds = 40.0f;
	float LocalJitterMilliseconds = 10.0f;
	float LocalLossPercent = 0.0f;
};
