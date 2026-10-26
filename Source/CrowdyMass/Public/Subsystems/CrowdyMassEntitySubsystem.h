#pragma once

#include "CoreMinimal.h"
#include "Assets/CrowdyMassHitZoneTable.h"
#include "Backend/CrowdyMassRenderingBackendConfig.h"
#include "Engine/TimerHandle.h"
#include "InstanceDataTypes.h"
#include "Interpolation/CrowdyInterpolationTiming.h"
#include "Mass/CrowdyMassEntity.h"
#include "Mass/EntityHandle.h"
#include "MassEntityManager.h"
#include "Reflection/CrowdyMassStateFieldType.h"
#include "Replication/RPC/FCrowdyEventParams.h"
#include "Replication/RPC/ICrowdyEntityEventHandler.h"
#include "Replication/Subsystems/ICrowdyEntitySubscriber.h"
#include "Settings/CrowdyMassDeveloperSettings.h"
#include "StructUtils/InstancedStruct.h"
#include "Subsystems/WorldSubsystem.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "Templates/SubclassOf.h"
#include "Templates/UniquePtr.h"
#include "UObject/ObjectKey.h"
#include "CrowdyMassEntitySubsystem.generated.h"

class UCrowdyEntitySubsystem;
class UCrowdyEventRouter;
class UCrowdyMassAnimSet;
class UCrowdyMassEntity;
class UMassEntityConfigAsset;
class UMassEntitySubsystem;
class UMassRepresentationSubsystem;
class UMassSpawnerSubsystem;
class UInstancedSkinnedMeshComponent;
class UCrowdyMassHitZoneTable;
class UPhysicsAsset;
class USkinnedAsset;
class FProperty;
class FStructProperty;
struct FCrowdyRepLayout;
struct FMassEntityTemplate;
struct FPropertyChangedEvent;

/**
 * The smallest number of avatars the cache will hold before it starts reclaiming, whatever the crowd
 * size. The working capacity is this or the tracked-entity count, whichever is larger; see
 * UCrowdyMassEntitySubsystem::GetAvatarCacheCapacity.
 *
 * An avatar is not a cached UObject that can be rebuilt for free, and the capacity is set from the
 * tracker for that reason. Reclaiming one drops the entity's record in the SDK entity registry, so a
 * reclaim costs a re-registration to get back: the entity reports ECrowdyMassAvatarState::Evicted until
 * something asks again, and any caller already holding a pointer to the reclaimed avatar keeps an object that no longer
 * receives anything. A cap
 * BELOW the tracked-entity count would spend that cost to save memory that does not matter,
 * and it would spend it on exactly the entities under sustained fire, which are the ones being touched
 * most often.
 *
 * The tracker is already a bound: an avatar is only ever minted for an id the tracker holds, and
 * UnregisterEntity releases it when that entity goes, so the cache cannot outgrow the crowd. This floor
 * only matters for a world with a handful of entities, where it costs a few hundred small objects at
 * most and saves reclaiming anything at all.
 */
inline constexpr int32 CrowdyMassMinCachedAvatars = 512;

/**
 * Inbound event allowance per entity: how many may be delivered per second, and how large a burst may
 * arrive at once.
 *
 * This is CPU protection and it is NOT anti-cheat. It exists so that a peer sending nonsense as fast
 * as it can cannot make this client spend its frame resolving and dispatching; it makes no judgement
 * about who sent anything, and it cannot, because the relay does not yet bind a sender to the entity
 * an event names.
 *
 * It is keyed on the TARGET entity, which is the only key available here, and that is exactly why the
 * rate is set high. A low ceiling on a target-keyed bucket is a griefing tool rather than a defence:
 * anyone able to send events at a player could then spend the player's own budget and silence their
 * animations for everyone. The rate is therefore set far above what legitimate play produces, so it
 * only ever engages against a flood, and a flood is the one case where dropping is the right answer.
 */
inline constexpr float CrowdyMassReceiveEventsPerSecond = 30.0f;
inline constexpr float CrowdyMassReceiveBurstEvents = 60.0f;

/**
 * How long an entity that has left keeps its avatar, and with it the entity registry record.
 *
 * Keeps a departed entity's avatar and registry record briefly, so an entity that flickers out and back
 * (a tier change, a brief drop) reuses its avatar instead of reminting it.
 */
inline constexpr float CrowdyMassAvatarReclaimGraceSeconds = 15.0f;

/**
 * What the avatar cache can say about one entity id.
 *
 * Evicted is a separate answer from NoEntity on purpose. "I threw your avatar away to stay under the
 * cap" and "that entity does not exist" look identical from a null pointer, and a caller that cannot
 * tell them apart takes the wrong action for one of them: it either gives up on a live entity or keeps
 * asking after a dead one. The two are different facts and are reported as different facts.
 */
UENUM(BlueprintType)
enum class ECrowdyMassAvatarState : uint8
{
	// No entity with this id is tracked here: it was never known, or it has been destroyed.
	NoEntity,

	// The entity is live and nothing has ever asked for an avatar for it.
	NotMinted,

	// The entity is live and its avatar exists right now.
	Present,

	// The entity is live and its avatar was reclaimed. Asking again mints a fresh one and re-enrols the
	// entity in the SDK entity registry; nothing about the entity itself has changed, but the record went
	// with the avatar and has to be made again.
	Evicted
};

USTRUCT()
struct CROWDYMASS_API FCrowdyMassAvatarEntry
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UCrowdyMassEntity> Avatar = nullptr;

	// A monotonic counter rather than a clock. Eviction needs only an order, and a counter gives a total
	// one for free: it cannot tie, cannot be perturbed by a clock adjustment, and costs no timer read on
	// the lookup path, which runs far more often than eviction does.
	UPROPERTY()
	int64 LastTouch = 0;
};

// An avatar whose entity has left, kept for a while in case the same entity is drawn again. Holding the
// avatar is what holds its entity registry record: a grace that released the object would still have to
// register the entity all over again.
USTRUCT()
struct CROWDYMASS_API FCrowdyMassAvatarGraceEntry
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UCrowdyMassEntity> Avatar = nullptr;

	// On the same clock as FPlatformTime::Seconds.
	UPROPERTY()
	double ExpiresAtSeconds = 0.0;
};

// One entity's inbound event allowance. Plain data: it holds no object and is keyed by an id that is
// already known to the tracker, so it needs no reflection and no lifetime of its own.
struct FCrowdyMassReceiveBudget
{
	float Tokens = 0.0f;

	// On the same clock as FPlatformTime::Seconds. Zero means the bucket has never been charged, which
	// is how a fresh bucket is told apart from one that has simply not been touched this second.
	double LastRefillSeconds = 0.0;
};

struct FCrowdyMassSpatialGridEntry
{
	FVector Origin = FVector::ZeroVector;
	float Radius = 0.0f;
	TArray<FIntVector> Cells;
};

struct FCrowdyMassTraceCandidateCell
{
	float EntryDistance = 0.0f;
	FBox DebugBounds = FBox(ForceInit);
	const TSet<FMassEntityHandle>* Entities = nullptr;
};

struct FCrowdyMassTraceInstanceCacheEntry
{
	TWeakObjectPtr<UMassRepresentationSubsystem> RepresentationSubsystem;
	TWeakObjectPtr<UInstancedSkinnedMeshComponent> Component;
	TWeakObjectPtr<USkinnedAsset> SkinnedAsset;
	TWeakObjectPtr<UPhysicsAsset> PhysicsAsset;
	FPrimitiveInstanceId InstanceId;
	FGuid UUID;
	int32 SkinnedMeshInfoIndex = INDEX_NONE;
	float LODSignificance = 0.0f;
};

/**
 * What an ingestor is given besides its own field's value: the entity being updated, the manager it
 * lives in, and a memo of the fragments already resolved for it on this update.
 *
 * An ingestor MUST NOT change the entity's archetype. Adding or removing a fragment, adding or
 * removing a tag, and destroying the entity are all exactly that. Beyond the standing rule that a
 * forgeable value may only ever produce a visual, GetFragment below hands out pointers into Mass chunk
 * storage, and the two ways of breaking that fail very differently. An ingestor that MOVES the entity
 * between archetypes leaves every later field of this update writing through a pointer into the
 * archetype the entity has just left. An ingestor that DESTROYS it is worse: a destroyed entity has no
 * archetype at all, and resolving a fragment for one asserts in a build with checks and dereferences
 * null in a build without, so the first later field asking for a type this context has not already
 * memoised takes the client down outright. Only the types already memoised degrade to the stale
 * pointer above. Trace collision is likewise off limits.
 */
struct FCrowdyMassViewStateContext
{
	FMassEntityManager& EntityManager;

	// Const so one context can never be repointed at a second entity. The memo below is keyed on
	// fragment type alone, so a caller that reused a context across a batch of entities would be handed
	// the first entity's fragment pointers for every later one, landing a remote peer's echo on the
	// wrong player with nothing anywhere to notice it. Ingestors only ever read this.
	const FMassEntityHandle Entity;

	// The class this entity was activated as, or null when the caller has none to give. Locally resolved
	// and never read off the wire, so an ingestor may use it to reach authored data for this class: the
	// pose field reaches the class's animation set through it, since a value's meaning is fixed by the
	// enum that class's set declares rather than by anything the sender said.
	//
	// Null is a real answer and not a failure. The view-state echo path resolves fields against a wire
	// struct that several classes can send, so it has no one class to name here, and an ingestor wanting
	// one must handle its absence rather than assume it.
	const UClass* const EntityClass;

	FCrowdyMassViewStateContext(FMassEntityManager& InEntityManager, const FMassEntityHandle InEntity,
		const UClass* const InEntityClass = nullptr)
		: EntityManager(InEntityManager)
		, Entity(InEntity)
		, EntityClass(InEntityClass)
	{
	}

	/**
	 * This entity's fragment of the given type, or null when it carries none, resolved once per update
	 * and handed straight back to every later caller.
	 *
	 * The memo is what lets several registered fields land on one fragment for a single archetype and
	 * chunk lookup instead of one lookup per field. At a few thousand entities updating ten times a
	 * second, resolving per field is the same lookup tens of thousands of times a second for one
	 * answer. A MISS is memoised exactly like a hit: an entity that carries no such fragment is
	 * ordinary, and re-resolving a null for every field is the same waste.
	 *
	 * The memo is a fixed-size inline table and never allocates. Once it is full, a further distinct
	 * type is resolved afresh on every call rather than evicting an entry, so an update touching more
	 * types than the table holds pays the lookup for the extra ones and the memoised types keep their
	 * answers.
	 */
	template<typename FragmentType>
	FragmentType* GetFragment() const
	{
		// Resolved once per type for the life of the process rather than called per lookup. It looks
		// like a plain accessor, but StaticStruct() is a call into another module that ends in a checked
		// cast, so it is neither free nor inlined away, and calling it here would make even a memo HIT
		// pay for it. What it returns cannot change while the process runs, so a function-local static
		// leaves every later call reading a pointer behind a guard variable.
		static const UScriptStruct* const Type = FragmentType::StaticStruct();
		for (int32 Index = 0; Index < ResolvedFragmentCount; ++Index)
		{
			if (ResolvedFragments[Index].Type == Type)
			{
				return static_cast<FragmentType*>(ResolvedFragments[Index].Fragment);
			}
		}

		FragmentType* Fragment = EntityManager.GetFragmentDataPtr<FragmentType>(Entity);
		if (ResolvedFragmentCount < MaxResolvedFragments)
		{
			ResolvedFragments[ResolvedFragmentCount].Type = Type;
			ResolvedFragments[ResolvedFragmentCount].Fragment = Fragment;
			++ResolvedFragmentCount;
		}

		return Fragment;
	}

private:
	// Small on purpose: an update resolves the handful of fragments its registered fields land on, and
	// a table this size costs a few pointers on the stack rather than an allocation on the network
	// arrival path.
	static constexpr int32 MaxResolvedFragments = 4;

	struct FResolvedFragment
	{
		const UScriptStruct* Type;
		void* Fragment;
	};

	// Deliberately left uninitialised, and ResolvedFragmentCount is what makes that safe: the scan
	// never looks past the count, and an entry is only counted once both of its members have been
	// written. A context is built afresh for every entity of every update, so initialising the table
	// would be eight pointer stores per update on the network arrival path for values nothing can ever
	// read. Do not add member initialisers back without first giving the scan another bound.
	//
	// Mutable because resolving a fragment changes nothing an ingestor can observe, and the context is
	// handed to every ingestor by const reference.
	mutable FResolvedFragment ResolvedFragments[MaxResolvedFragments];
	mutable int32 ResolvedFragmentCount = 0;
};

/**
 * One field of a view-state payload (the pose id, and whatever a consuming game registers of its
 * own): given the field's value straight off the wire struct and the entity to land it on, masks and
 * clamps whatever it needs to and writes the result onto that entity's fragments.
 *
 * Every byte behind PropertyValuePtr came off the network from the entity's own client and is
 * forgeable, so an ingestor may only ever produce a visual. See FCrowdyMassViewStateContext above for
 * what it must not do.
 *
 * PropertyValuePtr already points at this field's value inside the wire struct instance (the
 * property's offset has been applied), not at the struct's base address, so an ingestor never needs
 * to know anything about the struct it came from, only the C++ type of the one field it registered
 * for.
 */

using FCrowdyMassViewStateIngestor = TFunction<void(const void* PropertyValuePtr, const FCrowdyMassViewStateContext& Context)>;

// One registered view-state field, in registration order.
struct FCrowdyMassViewStateFieldRegistration
{
	FName PropertyName;
	FCrowdyMassViewStateIngestor Ingestor;
};

// One registration's resolution against a particular wire UScriptStruct. Null means that struct does
// not carry this field, which is the ordinary case for a game's own field on a struct another game's
// producer sends, or for any field on a struct that predates it. It also means the struct carries the
// NAME but not as a single byte, which is refused rather than read: see ResolveViewStateStruct.
struct FCrowdyMassViewStateResolvedField
{
	const FProperty* Property = nullptr;
};

// One wire UScriptStruct's resolution, parallel to the subsystem's ViewStateFieldRegistrations array
// as it stood when this entry was built.
struct FCrowdyMassViewStateResolvedStruct
{
	TArray<FCrowdyMassViewStateResolvedField> Fields;
};

// A resolution is handed out by shared reference so that an update already walking one cannot have it
// freed underneath it. The reference count is deliberately NOT the default atomic one: registering a
// field and applying an echo are both game thread only (the update queue is filled from a worker thread
// and drained on tick, the spawn broadcast hops to the game thread before it is delivered, and the
// registry's own registration asserts the thread), so an atomic pair would be paid on every update to
// guard against a race that cannot occur. Do not drop the mode back to the default: this reference is
// taken tens of thousands of times a second on the network arrival path.
using FCrowdyMassViewStateResolvedStructRef = TSharedRef<FCrowdyMassViewStateResolvedStruct, ESPMode::NotThreadSafe>;
using FCrowdyMassViewStateResolvedStructConstRef = TSharedRef<const FCrowdyMassViewStateResolvedStruct, ESPMode::NotThreadSafe>;

/**
 * One field CrowdyMass reads off a CrowdyState delta decoded for an entity this subsystem holds as a row
 * (see ICrowdyEntitySubscriber). Mirrors FCrowdyMassViewStateFieldRegistration; the difference is only
 * what it is resolved against, an FCrowdyRepLayout rather than a wire UScriptStruct.
 */
enum class ECrowdyMassStateFieldMatch : uint8
{
	// Claims the layout slot whose property carries PropertyName. What a game's own field uses: the game
	// knows what it called its property and registers under that name.
	ByName,

	// Claims the layout slot whose property is declared as the enum the class's animation set names in
	// PoseStateEnum, whatever the property is called. What CrowdyMass's own pose field uses, so that a
	// game names its state enum once, on the set, and is then free to call the property anything.
	ByPoseStateEnum,
};

struct FCrowdyMassStateFieldRegistration
{
	// Meaningful only for ByName. A registration matched by type carries None, because there is no name
	// it is looking for and printing one in a diagnostic would name a property nobody has to declare.
	FName PropertyName;
	FCrowdyMassViewStateIngestor Ingestor;

	// The type the ingestor reads its value as, and therefore the type a matched property has to be
	// declared as. Deliberately undefaulted: a registration that did not say what it reads is a type claim
	// nobody made, and the resolution would have nothing to check the ingestor's cast against.
	//
	// Meaningful only for ByName. The pose field is read as a single byte by its own check in
	// ResolveStateFragmentClass and does not consult this.
	ECrowdyMassStateFieldType FieldType;

	ECrowdyMassStateFieldMatch Match = ECrowdyMassStateFieldMatch::ByName;

	// True only for CrowdyMass's own fields (registered by RegisterStandardStateFragmentFields), never for
	// a game's own. A game's field being absent from another game's class is ordinary, exactly like the
	// view-state echo's per-struct resolution, but a class routed through this subsystem is expected to
	// carry every field CrowdyMass itself considers standard, so that one's absence is worth a diagnostic.
	bool bIsStandard = false;
};

// One class's resolution of every registered CrowdyState fragment field against its FCrowdyRepLayout,
// parallel to the subsystem's StateFieldRegistrations array as it stood when this entry was built.
// RegistrationIndexByLayoutIndex runs parallel to Layout.Properties instead: entry i is the index into
// StateFieldRegistrations that claims layout slot i, or INDEX_NONE when no registered field does. That is
// the flat table the apply loop indexes, built once per class rather than once per delta.
struct FCrowdyMassStateResolvedClass
{
	int64 LayoutHash = 0;
	TArray<int32> RegistrationIndexByLayoutIndex;
};

// Shared rather than stored by value for the same reason as FCrowdyMassViewStateResolvedStructRef: a
// registration can arrive from inside an ingestor and empties the cache, so the apply loop keeps its own
// reference to the entry it is walking. Not the default atomic reference mode for the same reason too:
// both registration and apply are game-thread only.
using FCrowdyMassStateResolvedClassRef = TSharedRef<FCrowdyMassStateResolvedClass, ESPMode::NotThreadSafe>;
using FCrowdyMassStateResolvedClassConstRef = TSharedRef<const FCrowdyMassStateResolvedClass, ESPMode::NotThreadSafe>;

/**
 * One replicated property of one class. BOTH halves are the key: two classes are free to replicate a
 * property of the same name and each one is its own subject to report on.
 *
 * The class is held as an FObjectKey so a collected class cannot be recycled into a stale hit.
 */
struct FCrowdyMassClassPropertyKey
{
	FObjectKey EntityClass;
	FName PropertyName;

	bool operator==(const FCrowdyMassClassPropertyKey& Other) const
	{
		return EntityClass == Other.EntityClass && PropertyName == Other.PropertyName;
	}

	friend uint32 GetTypeHash(const FCrowdyMassClassPropertyKey& Key)
	{
		return HashCombine(GetTypeHash(Key.EntityClass), GetTypeHash(Key.PropertyName));
	}
};

// One CrowdyState property that decodes correctly and that no registered fragment field claims, so its
// value is thrown away for every entity of that class. Detections counts every resolution that decided it,
// while the log line is written only for the first, which is what lets the verdict report the whole figure
// rather than the number of lines a reader happened to see.
//
// The names are copied rather than resolved from the key when the verdict prints, because the verdict may
// be run long after a class has gone and a record naming nothing would be worse than no record.
struct FCrowdyMassDroppedStateProperty
{
	FString ClassName;
	FName PropertyName;
	int64 Detections = 0;
};

/**
 * What one inbound CrowdyEvent does to a crowd entity's representation: given the call's decoded
 * parameters and the entity to land them on, writes whatever the event means for what is drawn.
 *
 * This is the receive half of an event a game declares on its own ACTOR class. On the machine that owns
 * the entity the function body runs on the real actor and nothing here is involved; on a machine that
 * holds the same entity as a row in a table there is no instance to run a body on, so the game registers
 * what the event should do to the representation instead and it is run here.
 *
 * Every value in Params came off the network from the client being observed, so a handler may only ever
 * produce a visual, exactly as for the two view-state surfaces. The archetype-stability constraint is the
 * same as well: see FCrowdyMassViewStateContext for what a handler must not do to the entity.
 *
 * A handler writes fragments and returns. It must not loop over other entities: it runs per event on the
 * game thread, which is precisely the cost the crowd exists to avoid. Anything batched belongs in a
 * processor afterwards.
 */
using FCrowdyMassEventIngestor = TFunction<void(const FCrowdyEventParams& Params, const FCrowdyMassViewStateContext& Context)>;

/**
 * Names one registration so it can be removed again. Opaque: the value means nothing outside the world
 * that issued it, and an unregister given a handle from another world removes nothing.
 */
struct FCrowdyMassEventHandlerHandle
{
	uint64 Id = 0;

	bool IsValid() const { return Id != 0; }
};

/**
 * One value an object-bound handler copies out of an inbound call: which parameter it is read from on the
 * event, and which parameter it is written to on the handler.
 *
 * Resolved once when the binding is made, so applying a call is a flat walk of this table with no name
 * lookup in it. A parameter the handler declares that the event does not, or declares as another type, is
 * simply absent from the table and keeps whatever the handler's own default is.
 *
 * Both pointers belong to the two functions the registration holds weakly, and are only ever read once
 * both of those have been found to still exist: a parameter list cannot outlive the function that owns it.
 */
struct FCrowdyMassEventParamCopy
{
	const FProperty* SourceParam = nullptr;
	const FProperty* DestParam = nullptr;
};

/**
 * One registered event handler, in registration order within the function it was registered for.
 *
 * Two forms, and they are deliberately equals rather than one being a special case of the other. A handler
 * registered from C++ carries an Ingestor and writes fragments; a handler bound to an object's reflected
 * function is called with the entity target and the call's parameters, and can do anything a Blueprint can
 * do. Both are dispatched from the same list, in the order they were registered.
 *
 * The function is held weakly so a registration can never keep a class alive, and so a function that has
 * gone is detectable rather than dereferenced. The bound object is held weakly for the stronger reason
 * that a binding must stop meaning anything the moment the object it names is destroyed.
 */
struct FCrowdyMassEventHandlerRegistration
{
	TWeakObjectPtr<const UFunction> Function;
	FCrowdyMassEventHandlerHandle Handle;

	// Set for a handler registered from C++, and unset for one bound to an object's reflected function.
	FCrowdyMassEventIngestor Ingestor;

	// Set only for an object binding. The name is kept beside the resolved function because it is what
	// identifies the binding to an unregister, which has only the object and the name to go on.
	TWeakObjectPtr<UObject> BoundObject;
	TWeakObjectPtr<UFunction> BoundFunction;
	FName BoundFunctionName;

	// The handler's first parameter, which carries the entity the call arrived for. Required, and checked
	// when the binding is made.
	const FStructProperty* BoundTargetParam = nullptr;

	TArray<FCrowdyMassEventParamCopy> BoundParamCopies;

	bool IsObjectBinding() const { return !BoundFunctionName.IsNone(); }
};

/**
 * A registration is held by shared reference so that removing one cannot destroy the one currently
 * running: a handler can reach the register or unregister call while it is itself executing, and a
 * dispatch keeps its own reference to every handler it is about to run.
 *
 * Not the default atomic reference mode, for the same reason as the view-state resolutions above:
 * registering and dispatching are both game thread only, so an atomic pair would guard against a race
 * that cannot occur.
 */
using FCrowdyMassEventHandlerRef = TSharedRef<FCrowdyMassEventHandlerRegistration, ESPMode::NotThreadSafe>;

/** The row of UUID was recorded as Class: it appeared, or now stands for another class. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCrowdyMassRowClassRecorded, const FGuid& /*UUID*/, const UClass* /*Class*/);

/** The row of UUID left the tracker. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCrowdyMassRowRemoved, const FGuid& /*UUID*/);

/** Which Mass template a remote entity is spawned from. */
UENUM()
enum class ECrowdyMassRemoteEntitySource : uint8
{
	// The Mass Entity Config a project configured, which is what every ordinary spawn uses.
	ConfiguredAsset,

	// A template this plugin composes in code from its own trait, referencing no project asset at
	// all. For a caller that has to spawn an entity whose behaviour depends on nothing a project
	// happens to have authored.
	PluginDefaults
};

/**
 * Which authored source supplied a class's Mass entity config, in the order they are consulted.
 *
 * None is the interesting one: it is the state in which a spawn falls through to building a config out
 * of what the class itself carries, so it is what says a class depends on deriving rather than on
 * anything a project authored.
 */
UENUM()
enum class ECrowdyMassEntityConfigSource : uint8
{
	// Nothing authored answers for this class.
	None,

	// A binding for this exact class on the rendering backend config, which wins over everything below.
	PerClassBinding,

	// A binding for an ANCESTOR of this class, reached because the class itself is not bound. Kept apart
	// from the exact case because the two send an author to different places: an exact binding is a row
	// naming their class, an inherited one is a row naming a class they have to walk up to find, and a
	// report that called both "a per-class binding" would tell them to look for a row that is not there.
	InheritedClassBinding,

	// The rendering backend's own override, which covers every class it does not bind individually.
	BackendOverride,

	// The project-wide Remote Entity Config, in Project Settings.
	ProjectSetting
};

/**
 * Which source supplied a class's animation set, or why none did.
 *
 * The ways an answer can be absent are kept apart on purpose. A class nothing has asked about, a class
 * that names no set anywhere, and a class that names one which would not load are three different faults
 * with three different fixes, and every one of them hands back a null set. Collapsing them into one
 * branch turns "nobody has looked" into "there is nothing there", which is a confident wrong answer.
 */
UENUM()
enum class ECrowdyMassAnimSetSource : uint8
{
	// No class was given, so there was nothing to answer for. A caller reaching this is missing the class,
	// not missing an animation set.
	NoClass,

	// Nothing has asked about this class in this world yet, so no answer has been worked out. Only
	// GetResolvedAnimSetSource reports this; asking for the set itself works one out.
	NotResolved,

	// Asked, and nothing names a set for this class: no Mass entity config authored for it carries an
	// animation trait naming one, and the class itself carries no Crowdy Mass Anim Set component naming
	// one either.
	Undeclared,

	// A set IS named for this class and the reference did not load. The class is configured and the asset
	// is missing, renamed or unbuilt, which is an entirely different fix from authoring one.
	LoadFailed,

	// The animation trait on the Mass entity config authored for this class.
	ConfigTrait,

	// The Crowdy Mass Anim Set component on the class itself, which is what a class with no authored
	// config renders through.
	ClassComponent,

	// The class names a set on its own component AND a Mass entity config is authored for the class, so
	// entities of it are built from that config. The config installs no animation trait naming this set,
	// so nothing renders it: the entities carry either no animation fragments at all or one holding no
	// set. Kept apart from ClassComponent because every diagnostic that exists to explain a crowd standing
	// in its bind pose reads this answer, and reporting a plain success here is the confident wrong answer
	// that sends whoever is reading it to look somewhere else.
	ClassComponentNotRendered
};

/**
 * One class's animation set and which source supplied it, kept together so that neither can be read
 * without the other and both are dropped by the same invalidation.
 */
USTRUCT()
struct CROWDYMASS_API FCrowdyMassClassAnimSet
{
	GENERATED_BODY()

	// Held as a strong reference because this cache is the only thing keeping a set alive for a class that
	// was asked about but never spawned; a spawned class's set is also held by the shared fragment its
	// trait installs.
	UPROPERTY()
	TObjectPtr<const UCrowdyMassAnimSet> AnimSet = nullptr;

	UPROPERTY()
	ECrowdyMassAnimSetSource Source = ECrowdyMassAnimSetSource::Undeclared;
};

/**
 * What one class's near tier shell declaration did to the Mass entity config authored for it.
 *
 * AuthoredConfig is part of the answer rather than a note about it. Which config answers for a class is
 * decided by a per-class binding, a backend override and a project setting, and a backend swap moves all
 * three; an entry that recorded only the result would keep naming a config nothing points at any more.
 * Holding what it was built FROM lets a lookup check its own key in one pointer comparison, which is what
 * makes it safe against an invalidation nobody remembered to add.
 *
 * A null PatchedConfig with a non-null AuthoredConfig is a real answer and not an empty slot: it means
 * this class was asked about and needs no patch. Keeping it is what stops a class walk and a synchronous
 * class load happening once per entity for every class that declares no shell, which is most of them.
 */
USTRUCT()
struct CROWDYMASS_API FCrowdyMassNearTierPatchedConfig
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<const UMassEntityConfigAsset> AuthoredConfig = nullptr;

	UPROPERTY()
	TObjectPtr<UMassEntityConfigAsset> PatchedConfig = nullptr;
};

/**
 * A per-class dispatch stand-in and the properties returning it to class defaults has to copy.
 *
 * The property list is the whole reason this is a struct rather than a bare proxy pointer. Deciding which
 * properties to copy means walking the class's entire property chain and testing every object property for
 * whether it points at one of the proxy's own subobjects, and that answer cannot change while the proxy is
 * the same object of the same class. Held here, it is paid once per class instead of once per inbound event.
 */
USTRUCT()
struct FCrowdyMassDispatchProxyEntry
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<AActor> Proxy = nullptr;

	// The proxy's own subobjects, restored into SubobjectProperties by index on every reset. Reflected so a
	// component the proxy owns cannot be collected out from under the entry.
	UPROPERTY()
	TArray<TObjectPtr<UObject>> SubobjectValues;

	// Raw because they are only ever read between a proxy being built and this entry being dropped, and both
	// halves go together when reinstancing replaces the class the properties belong to.
	TArray<FProperty*> ResetProperties;
	TArray<FProperty*> SubobjectProperties;
};

/**
 * A pose name asked about for one entity class. BOTH halves are the key, because a pose name is a row in
 * that class's own animation set and two classes are free to author the same name as different poses, or
 * for one of them not to author it at all.
 *
 * The class is held as an FObjectKey so a collected class cannot be recycled into a stale hit. A default
 * key stands for "no class was given", which is a real thing to be asked and a real thing to answer.
 */
struct FCrowdyMassClassPoseKey
{
	FObjectKey EntityClass;
	FName PoseName;

	bool operator==(const FCrowdyMassClassPoseKey& Other) const
	{
		return EntityClass == Other.EntityClass && PoseName == Other.PoseName;
	}

	friend uint32 GetTypeHash(const FCrowdyMassClassPoseKey& Key)
	{
		return HashCombine(GetTypeHash(Key.EntityClass), GetTypeHash(Key.PoseName));
	}
};

UCLASS(BlueprintType, meta=(DisplayName="Crowdy Mass Entity Subsystem"))
class CROWDYMASS_API UCrowdyMassEntitySubsystem : public UWorldSubsystem, public ICrowdyEntitySubscriber,
	public ICrowdyEntityEventHandler
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/** Whether World draws Crowdy crowd rows: it is a play world whose map profile selects the Mass backend. */
	static bool DrawsCrowdRows(const UWorld* World);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass")
	FMassEntityHandle SpawnRemoteEntity(const FGuid& UUID, const FTransform& InitialTransform);

	// StateEntityClass is the class UCrowdyActorManager::ResolveEntityClass already resolved locally for
	// this entity, or null when the caller has none to give. It is stored verbatim and handed back from
	// GetEntityClass below; nothing off the wire ever chooses it. Null clears any class held for this UUID
	// from a previous call, so a caller that genuinely has no class does not leave a stale one behind. It
	// does NOT make the entity unknown: an entity with no class is tracked exactly like any other.
	FMassEntityHandle SpawnRemoteEntity(const FGuid& UUID, const FTransform& InitialTransform, const FInstancedStruct& InitialState, const UClass* StateEntityClass = nullptr,
		ECrowdyMassRemoteEntitySource Source = ECrowdyMassRemoteEntitySource::ConfiguredAsset);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass")
	void DestroyRemoteEntity(const FGuid& UUID);

	UFUNCTION(BlueprintCallable, Category="Crowdy Mass")
	bool UpdateRemoteEntityTransform(const FGuid& UUID, const FVector& Location, const FRotator& Rotation, int64 ServerTimestampMs, float SmoothingDuration = -1.0f);

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass")
	bool FindEntityByUUID(const FGuid& UUID, FMassEntityHandle& OutEntity) const;

	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass")
	bool FindUUIDByEntity(FMassEntityHandle Entity, FGuid& OutUUID) const;

	void GetTrackedRemoteEntities(TArray<FMassEntityHandle>& OutEntities) const;
	int32 GetTrackedRemoteEntityCount() const;
	bool GetTraceCandidateEntities(const FVector& Start, const FVector& End, TArray<FMassEntityHandle>& OutEntities, int32& OutVisitedCellCount) const;
	bool GetTraceCandidateEntities(const FVector& Start, const FVector& End, float QueryRadius, TArray<FMassEntityHandle>& OutEntities, int32& OutVisitedCellCount) const;
	bool GetTraceCandidateEntities(const FVector& Start, const FVector& End, float QueryRadius, TArray<FMassEntityHandle>& OutEntities, int32& OutVisitedCellCount, TArray<FBox>* OutVisitedCellBounds) const;
	bool GetTraceCandidateCells(const FVector& Start, const FVector& End, float QueryRadius, TArray<FCrowdyMassTraceCandidateCell>& OutCells, int32& OutVisitedCellCount, TArray<FBox>* OutVisitedCellBounds = nullptr) const;
	bool ForEachTraceCandidateCell(const FVector& Start, const FVector& End, float QueryRadius, TFunctionRef<bool(const FCrowdyMassTraceCandidateCell& Cell, bool bTraversalIsEntrySorted)> Visitor, int32& OutVisitedCellCount) const;
	bool GetOverlapCandidateEntities(const FVector& Origin, float QueryRadius, TArray<FMassEntityHandle>& OutEntities, int32& OutVisitedCellCount) const;
	const FCrowdyMassTraceInstanceCacheEntry* FindTraceInstanceCache(FMassEntityHandle Entity) const;
	void UpdateTraceInstanceCache(FMassEntityHandle Entity, const FCrowdyMassTraceInstanceCacheEntry& Entry);
	void RemoveTraceInstanceCache(FMassEntityHandle Entity);
	void ClearTraceInstanceCache();

	/**
	 * Replaces the hit zone table trace results in THIS world are resolved against, walking it into a
	 * flat per-bone lookup once here rather than on every hit.
	 *
	 * World-scoped rather than process-wide because two worlds are alive at once during level travel and
	 * two PIE sessions run side by side: a shared table would let one session's assignment decide the
	 * other's hit zones, and would survive into the next PIE run, which no cold start can reproduce.
	 *
	 * Null clears it, which puts every bone back on the built-in default zone.
	 */
	void SetHitZoneTable(const UCrowdyMassHitZoneTable* HitZoneTable);

	// The flat lookup SetHitZoneTable last built for this world. Never null: a world that has never been
	// given a table resolves every bone to the built-in default zone.
	const FCrowdyMassResolvedHitZoneTable& GetResolvedHitZoneTable() const { return ResolvedHitZoneTable; }

	/**
	 * Says once, per world, that an entity was refused trace collision because it carries no trace
	 * collision fragment at all, and counts every such refusal.
	 *
	 * The fragment has exactly one producer, the Crowdy Mass Trace Collision trait, and nothing adds it
	 * when a remote entity is spawned. A crowd config that omits the trait therefore produces a crowd
	 * that is silently untraceable, which from the outside is indistinguishable from an empty spatial
	 * grid or a mis-aimed trace, so the trait is named rather than left to be guessed.
	 */
	void ReportMissingTraceCollisionFragment();

	// How many entities have been refused trace collision in this world for carrying no trace collision
	// fragment. Non-zero while a crowd is meant to be traceable means the trait is missing from its
	// config; the warning says so once, this says how much of the crowd it covers.
	int32 GetMissingTraceCollisionFragmentCount() const { return MissingTraceCollisionFragmentCount; }

	/**
	 * Working memory the trace path uses to remember which entities one trace has already tested,
	 * emptied and handed back so the caller starts clean and the storage it grew to survives.
	 *
	 * It lives here rather than in the trace library because that library is a static Blueprint function
	 * library: a container held there would be one container for the whole process, shared between two
	 * PIE sessions and between the two worlds alive during level travel. Only one trace may hold this at
	 * a time, which is what the trace path does.
	 */
	TSet<FMassEntityHandle>& ResetTraceTestedEntitiesScratch();

	/**
	 * Apply per-backend overrides (called by UCrowdyMassRenderingBackend). A null config asset
	 * or a negative smoothing duration falls back to the project-wide Crowdy Mass settings.
	 *
	 * The per-class bindings are RESOLVED AND FLATTENED HERE, once, into a pointer-keyed map. Doing it
	 * per spawn would mean loading a soft class and a soft object reference on a path that runs for
	 * every entity that appears, and doing it per update would put it in the hottest loop in the
	 * plugin. An entry naming a class that cannot be loaded is reported and dropped here, where the
	 * cause is visible, rather than presenting later as one kind of entity quietly using the wrong
	 * config.
	 */
	void ConfigureOverrides(const TSoftObjectPtr<UMassEntityConfigAsset>& InConfigOverride, float InSmoothingOverride,
		const TArray<FCrowdyMassEntityConfigBinding>& InPerClassConfigs = TArray<FCrowdyMassEntityConfigBinding>());

	/**
	 * The authored Mass entity config a spawn of EntityClass would use, and which of the three authored
	 * sources supplied it. Null and None mean nothing authored answers for that class, so a spawn would
	 * fall through to building a config out of what the class itself carries.
	 *
	 * For a caller reporting what WOULD happen without making it happen. It asks exactly the resolve a
	 * spawn asks rather than a copy of it, which is the point: a second implementation of the ordering
	 * would answer confidently and wrongly the first time the ordering changed.
	 */
	const UMassEntityConfigAsset* FindAuthoredEntityConfig(const UClass* EntityClass, ECrowdyMassEntityConfigSource& OutSource) const;

	/**
	 * The class whose per-class binding answers for EntityClass: EntityClass itself when it is bound,
	 * otherwise the nearest bound ancestor, otherwise null.
	 *
	 * For a caller that has to NAME the row an author would have to edit. The resolve reports whether a
	 * binding was exact or inherited, but not which class carries it, and an author told only that some
	 * ancestor answered still has the whole parent chain to search by hand.
	 */
	const UClass* FindEntityConfigBindingClass(const UClass* EntityClass) const;

	/**
	 * The sentence to hand an author who has been told an authored config is overriding something their
	 * class declares, telling them what to actually go and change.
	 *
	 * It has to be worked out rather than written into each message, because "stop binding the config to
	 * this class" is only followable when a row names this exact class. Three of the four ways a config
	 * can answer have no such row: an inherited binding is filed under an ancestor and deleting it changes
	 * every class beneath it, and the backend override and the project-wide setting are not per-class rows
	 * at all. A remedy naming a row that does not exist sends an author looking for it and ends with them
	 * deciding the warning is wrong.
	 */
	FString DescribeAuthoredConfigRemedy(const UClass* EntityClass) const;

	/**
	 * Extract a location/rotation (and optional smoothing duration) from a replicated state struct.
	 * Recognises FCrowdyMassTransformState and falls back to reflection (Transform, or Location/Position
	 * + Rotation). Returns false if no transform-like data is found.
	 *
	 * A struct that carries no readable transform is reported once per struct type, as a warning naming
	 * the struct and the field names and types this reads. Nothing else reports it: the layout verdict is
	 * worked out once and then answered from a memo, so without that warning a struct whose fields are
	 * spelled or typed differently simply produces entities that never move, silently.
	 */
	static bool TryExtractTransformState(const FInstancedStruct& State, FVector& OutLocation, FQuat& OutRotation, float& OutSmoothingDuration);

	/**
	 * The pose id an authored pose name maps to in EntityClass's animation set, or 0 for a name that is
	 * not authored there (and for NAME_None, which is the documented "no pose" sentinel).
	 *
	 * This is how anything outside the animation system names a pose without ever handling an index: an id
	 * is a position in an authored array, so hard-coding one anywhere else would silently mean a different
	 * pose the moment a row is inserted above it.
	 *
	 * THE CLASS IS PART OF THE QUESTION. Each class renders through its own animation set, so the same
	 * name is a different row, or no row at all, from one class to the next. A null class answers 0: it is
	 * not a class whose set happens to be empty, it is a caller with no class to ask about.
	 *
	 * The answer per (class, name) is cached, misses included, so a name asked about every frame costs one
	 * array search in the life of the world. Resolving a class for the first time loads that class's entity
	 * config and its animation set, so the very first call for a class is not free; every later one is.
	 *
	 * Game thread only. It writes its own cache, and the assets it reads are resolved on that thread.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass|Anim")
	int32 FindPoseId(const UClass* EntityClass, FName PoseName) const;

	/**
	 * The animation set entities of EntityClass render through, or null when nothing names one for it.
	 *
	 * Public so an ACTOR can resolve an action against the same authored rows the crowd renders for the
	 * same class. The class is required rather than optional: an action id is a row position in ONE set,
	 * so resolving it against another class's set would hand an actor of one class the clip authored for
	 * another, with nothing anywhere to notice.
	 *
	 * Null covers three different situations. Use FindRemoteEntityAnimSet when they need telling apart.
	 */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category="Crowdy Mass|Anim")
	const UCrowdyMassAnimSet* GetRemoteEntityAnimSet(const UClass* EntityClass) const;

	/**
	 * The same answer as GetRemoteEntityAnimSet, plus which source supplied it or why none did.
	 *
	 * For anything that reports a failure to a person rather than silently doing nothing: "this class
	 * names no set", "this class names one that would not load" and "no class was given" are three
	 * different things to fix and are otherwise all just a null pointer.
	 */
	const UCrowdyMassAnimSet* FindRemoteEntityAnimSet(const UClass* EntityClass,
		ECrowdyMassAnimSetSource& OutSource) const;

	/**
	 * Which source ALREADY answered for this class, without working an answer out.
	 *
	 * NotResolved is the answer this exists for, and it is the third of the three misses: nothing has
	 * asked about this class in this world, so its silence says nothing about whether it names a set. A
	 * diagnostic that resolved the class in order to report on it could never say that, because asking is
	 * what stops it being true.
	 */
	ECrowdyMassAnimSetSource GetResolvedAnimSetSource(const UClass* EntityClass) const;

	/**
	 * Drops the cached per-class animation set answer in every live world.
	 *
	 * For whoever changes, from outside any one world, which Mass entity config a spawn is built from. A
	 * cached answer is a row table in one particular set, so moving which set that is leaves an
	 * already-resolved class naming rows in a set nothing renders any more, while classes resolved after
	 * the change name rows in the new one. Pose and action ids are row positions, so the two disagree
	 * silently: an id resolved against the old set indexes a different row of the new one.
	 */
	static void InvalidateAnimSetCachesInEveryWorld();

	/**
	 * Drops the configs built from a class in every live world, so the next spawn of each class derives
	 * again. For an edit that bypasses the property-changed event, such as an undo of an anim set's Bank,
	 * which a derived config copies into its mesh description.
	 */
	static void InvalidateDerivedConfigsInEveryWorld();

	/**
	 * Puts a tracked entity into an authored pose by name, or clears its pose when PoseName is
	 * NAME_None. False when the id names no tracked entity, when that entity carries no pose fragment (an
	 * archetype with no animation trait has nothing to pose), and when a pose was named for an entity with
	 * no recorded class.
	 *
	 * The name is resolved against the animation set of the class this entity was recorded as, which the
	 * entity's own id already names, so the caller does not carry a class. An entity with no recorded
	 * class resolves no pose: there is no set to look the name up in, and guessing at one would put the
	 * entity into whatever row another class happens to author under that name. Such a call is REFUSED
	 * rather than answered with the "no pose" id, which would clear the entity's pose while reporting that
	 * the pose had been set. Clearing itself still succeeds, since it needs no set.
	 *
	 * This is presentation only, exactly like the pose that arrives over the echo: it changes what is
	 * drawn and nothing else. Setting a pose on an entity that also receives echoes is temporary by
	 * nature, since the next echo carrying a pose id overwrites it.
	 *
	 * Refused, and reported, while Mass processors are running, under the same rule every other
	 * Blueprint-reachable write of entity data follows: the write would race every processor declared to
	 * read the pose fragment. A tick that overlaps a Mass phase reaches here as readily as a graph run from
	 * inside a chunk loop, because phases run alongside the game thread rather than blocking it.
	 * UCrowdyMassTargetLibrary::SetEntityPose is the same write addressed by target instead of by id, and
	 * the two answer identically.
	 */
	UFUNCTION(BlueprintCallable, Category="Crowdy Mass|Anim")
	bool SetEntityPose(const FGuid& UUID, FName PoseName);

	/**
	 * Registers one field of the view-state payload. PropertyName is resolved by reflection against
	 * whatever wire struct a game sends, once per distinct UScriptStruct rather than per update (see
	 * ResolveViewStateStruct), so a game can carry its own continuous state in its own wire struct and
	 * land it in its own fragment without CrowdyMass knowing the type. CrowdyMass registers only two
	 * standard fields of its own (Version, whose byte is described in View/CrowdyMassViewState.h, and
	 * PoseId) through this exact call in Initialize(), so a game's own field is never special-cased
	 * ahead of the mechanism.
	 *
	 * Every byte an ingestor is handed came off the network from the client being observed and is
	 * forgeable, so an ingestor may only ever produce a visual. It may drive what is drawn; it may not
	 * be what anything is decided from, because a player who lies about the byte decides it instead.
	 *
	 * The ingestor must not change the entity's archetype, which means it must not add or remove a
	 * fragment or a tag and must not destroy the entity. FCrowdyMassViewStateContext hands out
	 * pointers into chunk storage that it memoises for the rest of the update. Moving the entity to
	 * another archetype leaves every later field of the same update writing through a pointer into the
	 * archetype it left; destroying it is worse, because a destroyed entity has no archetype at all, so
	 * the first later field asking for a type the context has not already memoised asserts in a build
	 * with checks and dereferences null in a build without.
	 *
	 * Lives on this world subsystem rather than a global: two worlds exist at once during level
	 * travel, and a global registry would let one world's fields answer for the other's decode.
	 */
	void RegisterViewStateField(FName PropertyName, FCrowdyMassViewStateIngestor Ingestor);

	// True when a field with this name has been registered. A game's gate or startup check can use this
	// to notice that its own ingestors never registered, which is otherwise silent: an unregistered
	// field simply never lands and the entity keeps its spawn defaults forever.
	bool IsViewStateFieldRegistered(FName PropertyName) const;

	/**
	 * Applies every registered view-state field found on State's script struct onto UUID's entity.
	 * A field the struct does not carry is left untouched, so the entity keeps whatever it already had
	 * (spawn defaults, or the value from a previous update) rather than being reset by a payload
	 * shaped for a different field set. False when UUID names no tracked entity.
	 *
	 * Call this before extracting a transform from the same payload: a transform that fails to
	 * resolve must not stop the view-state echo from landing.
	 */
	bool ApplyViewState(const FGuid& UUID, const FInstancedStruct& State);

	/**
	 * One received payload applied whole: the view-state fields first, then the transform it carries.
	 * False when UUID names no tracked entity.
	 *
	 * This is the per-entity receive door, and it resolves the UUID ONCE for both halves. Calling
	 * ApplyViewState and UpdateRemoteEntityTransform in turn is the same work with a second lookup and a
	 * second validation, per entity per update, on the path the population ceiling is bound by.
	 */
	bool ApplyRemoteEntityUpdate(const FGuid& UUID, const FInstancedStruct& State, int64 ServerTimestampMs);

	/**
	 * Registers one field CrowdyMass reads off a CrowdyState delta decoded for an entity that has no
	 * object of its own to decode onto (see ICrowdyEntitySubscriber). PropertyName is resolved by
	 * reflection against each entity class's FCrowdyRepLayout, once per distinct (class, layout hash)
	 * rather than per delta, mirroring RegisterViewStateField's resolution against a wire UScriptStruct.
	 *
	 * The same two properties are deliberate for the same reasons: BY NAME, never by position, because a
	 * layout is positional and a game adding a CrowdyState property above this one must not shift what
	 * lands; and WORLD-SCOPED, never global, because two worlds can be alive at once during level travel
	 * and one world's fields must not answer for the other world's decode.
	 *
	 * Every byte an ingestor is handed came off the network from the client being observed and is
	 * forgeable, so an ingestor may only ever produce a visual, exactly as for RegisterViewStateField. The
	 * same archetype-stability constraint applies too: see FCrowdyMassViewStateContext.
	 *
	 * FieldType is the type the ingestor reads the value as, and it is part of the match: a property
	 * carrying the name but declared as something else is refused and says so, rather than being handed to
	 * an ingestor whose cast would misread it. An enum is read as the type it is declared over. A property
	 * of a type ECrowdyMassStateFieldType does not list cannot be bound here at all and is read on the
	 * actor representation instead.
	 *
	 * A name is claimed process-wide across every class whose layout is resolved, and a registration
	 * carries one type, so a game declaring Health as an int32 on one class and a float on another can bind
	 * only one of them. The other is refused with a line naming both the registered type and the declared
	 * one; give the two properties different names to bind both.
	 */
	void RegisterStateFragmentField(FName PropertyName, ECrowdyMassStateFieldType FieldType, FCrowdyMassViewStateIngestor Ingestor);

	// True when a CrowdyState fragment field with this name has been registered. Mirrors
	// IsViewStateFieldRegistered.
	bool IsStateFragmentFieldRegistered(FName PropertyName) const;

	/**
	 * Registers what one CrowdyEvent does to a crowd entity's representation, keyed on the function a game
	 * already authored on its own ACTOR class.
	 *
	 * The actor class is the only class involved, and that is the point of this call. A game declares its
	 * event once, where the owner's real actor declares it, and calls it once; the owner's machine runs the
	 * body, and every observer holding that entity as a row runs what is registered here instead. There is
	 * no second class to author, no avatar subclass to keep in step with the actor, and no subsystem to
	 * write per feature: another event is another call to this.
	 *
	 * Function is the reflected receiver, which for an event declared through CROWDY_EVENT is the
	 * "<Name>_Implementation" function on the declaring class (FCrowdyRPC::ResolveFunction finds it). It is
	 * the exact function object that is matched, so a Blueprint class recompiled in the editor mints a new
	 * one and needs registering again; a call that finds nothing registered says so in the log rather than
	 * disappearing.
	 *
	 * HANDLERS COEXIST. Registering the same function again adds a second handler; both run, in the order
	 * they were registered, and no handler may depend on that order. One event genuinely has more than one
	 * meaning: writing a fragment so the entity is drawn flinching is one job, and printing a line, playing
	 * a sound or bumping a counter is another, and neither is a variant of the other. Use the returned
	 * handle to take a handler back off again; nothing is replaced implicitly.
	 *
	 * WORLD-SCOPED, never global, for the same reason as the other three registration surfaces: two worlds
	 * can be alive at once during level travel, and one world's handlers must not answer for the other's
	 * inbound calls.
	 *
	 * Returns an invalid handle when the registration was refused, which is only ever a null function or a
	 * null handler.
	 */
	FCrowdyMassEventHandlerHandle RegisterEventHandler(const UFunction* Function, FCrowdyMassEventIngestor Ingestor);

	/**
	 * Registers HandlerFunctionName on HandlerObject as a handler for one CrowdyEvent, which is what the
	 * "Receive Crowdy Event on Entity" Blueprint node compiles to.
	 *
	 * The handler's signature IS the contract, and it is checked here rather than when a call arrives: its
	 * first parameter must be an FCrowdyMassEntityTarget naming the entity the call was for, and its
	 * remaining parameters are matched to the event's parameters BY NAME and only where the two declare
	 * exactly the same type. A parameter the event does not carry, or carries as something else, keeps the
	 * handler's own default and is reported here, once, rather than being reinterpreted through the wrong
	 * property. The handler may declare no output parameter and no return value.
	 *
	 * The object is held WEAKLY and the binding is dropped the moment the object is destroyed, so a node
	 * bound to an actor stops meaning anything when that actor goes. Everything the handler is given came
	 * off the network from the client being observed and is forgeable, exactly as for a C++ handler.
	 *
	 * Binding the same object and function name again is the SAME binding rather than a second one: it is
	 * accepted, hands back the handle the first one produced, and adds nothing. A graph that reaches its
	 * bind node twice (a respawn, a re-entered state) must not start running its handler twice per event.
	 *
	 * Returns an invalid handle when the binding was refused, which always logs why.
	 */
	FCrowdyMassEventHandlerHandle RegisterObjectEventHandler(const UFunction* Function, UObject* HandlerObject,
		FName HandlerFunctionName);

	/**
	 * Removes one registration. True when it was there to remove.
	 *
	 * Safe to call from inside a handler, including on the handler doing the calling: the entry is taken
	 * out of the list the next dispatch reads, while the one currently executing is kept alive until it
	 * returns. It does not run again after this returns.
	 */
	bool UnregisterEventHandler(FCrowdyMassEventHandlerHandle Handle);

	// Removes the binding RegisterObjectEventHandler made for this object and function name. True when one
	// was there to remove. This is what an author's own unbind reaches, since the handle a bind produced is
	// not something a graph holds on to.
	bool UnregisterObjectEventHandler(const UFunction* Function, const UObject* HandlerObject,
		FName HandlerFunctionName);

	// True when at least one handler is registered for this function. A game's startup check can use it to
	// notice that its own handlers never registered.
	bool IsEventHandlerRegistered(const UFunction* Function) const;

	// How many handlers are registered for this function. Zero for a function nothing claims. Exists so a
	// game (or a test) can tell one registration from two, which IsEventHandlerRegistered cannot.
	int32 NumEventHandlersRegistered(const UFunction* Function) const;

	/**
	 * ICrowdyEntityEventHandler. Whether anything is registered to apply this function to an entity held
	 * here as a row.
	 *
	 * Answered from the function alone, and deliberately not from any entity: the router asks this while a
	 * call may still be waiting for its target to arrive, and an answer that needed the entity would be
	 * "no" for exactly the calls that are waiting.
	 */
	virtual bool HasEntityEventHandler(const UFunction* Function) const override;

	/**
	 * ICrowdyEntityEventHandler. Spends one token of the entity's inbound allowance for this call, or refuses.
	 *
	 * This is the ONLY place a token is spent for a call applied as data, and it deliberately runs before the
	 * parameter frame is decoded, so the allowance bounds the decoding as well as the handler.
	 *
	 * Every refusal that can be decided from the entity's class and the function alone is asked FIRST and costs
	 * nothing: an event aimed at the wrong kind of entity, and one whose body no stand-in of this entity's class
	 * could ever host. A call that cannot apply anything must not be able to spend the named entity's allowance,
	 * or a peer that knows nothing but an entity id can silence that entity's real events by streaming calls
	 * none of which was ever capable of landing.
	 */
	virtual bool ChargeEntityEventBudget(const FGuid& EntityUUID, const UFunction* Function) override;

	/**
	 * ICrowdyEntityEventHandler. Runs every handler registered for this call, in registration order,
	 * against the entity's own fragments.
	 *
	 * The entity is re-checked between handlers and each is given its own context. A handler bound to an
	 * object runs arbitrary game code, which can destroy the entity or move it between archetypes, and a
	 * context shared across handlers would hand a later one fragment pointers into storage the entity has
	 * already left.
	 *
	 * The entity's recorded class must derive the function's declaring class, which is an AUTHORING guard
	 * and not a security boundary: the recorded class traces back to what the entity's own sender said it
	 * was, so it catches an event aimed at the wrong kind of entity and nothing more. Every parameter stays
	 * forgeable regardless, which is why a handler may only ever produce a visual.
	 *
	 * Charges nothing: the allowance was already spent by ChargeEntityEventBudget above. The class check is
	 * asked again all the same, because this is a public entry point and a caller reaching it directly must
	 * get the same answer as one arriving through the router.
	 */
	virtual void HandleEntityEvent(const FGuid& EntityUUID, const FGuid& SenderID, const UFunction* Function,
		const FCrowdyEventParams& Params) override;

	/**
	 * ICrowdyEntitySubscriber. True when the tracker holds a live entity under this id, whatever else is
	 * or is not recorded for it.
	 *
	 * Answered from the tracker alone, and deliberately not from the avatar cache or from the recorded
	 * class. An entity whose avatar was reclaimed is still very much known, and so is one spawned with no
	 * class at all (a crowd that is only ever a position is a legitimate crowd). Reporting either as
	 * unknown would put every payload for it back on a queue it can only time out on, and a crowd of those
	 * fills the shared wait queue and evicts the entities that genuinely are still arriving.
	 */
	virtual bool IsEntityKnown(const FGuid& EntityUUID) const override;

	/**
	 * ICrowdyEntitySubscriber. The class this entity is a representation of, or null when none was
	 * recorded for it.
	 *
	 * Null here does NOT mean the id is unknown, and this is never the answer to whether it is: an entity
	 * spawned with no class is held exactly like any other and simply has no routing for the planes that
	 * need one. See the interface for why the answer must come from locally resolved, never wire-carried,
	 * data.
	 */
	virtual const UClass* GetEntityClass(const FGuid& EntityUUID) const override;

	/** After a row's class is recorded with a class other than the one it had. Listeners must not spawn or destroy rows. */
	FOnCrowdyMassRowClassRecorded OnRowClassRecorded;

	/** After a row leaves the tracker. Listeners must not spawn or destroy rows. */
	FOnCrowdyMassRowRemoved OnRowRemoved;

	/**
	 * The stand-in this plane runs an inbound event's own body on for entities of EntityClass, built on
	 * first use and kept for as long as the class is. Null when no proxy can be made of the class, which is
	 * a class that is abstract, is not an actor class, or belongs to no world.
	 *
	 * ONE PER CLASS, never one per entity, which is what makes this affordable at crowd sizes: dispatch is
	 * sequential on the game thread, so at most one proxy is ever in use and it is borrowed for a single
	 * event. Three character classes means three proxies whether there are ten players or two thousand.
	 * Contrast the avatar cache, which is capped and reclaimed with LRU precisely because it is one UObject
	 * per entity.
	 *
	 * Public so a test can ask whether the memo is a memo. Nothing else should hold the answer: it is
	 * borrowed for the duration of one dispatch and its property block is reset before the next.
	 */
	AActor* GetOrCreateDispatchProxy(const UClass* EntityClass);

	/**
	 * ICrowdyEntitySubscriber. The object an inbound event for this entity should be delivered to, or null
	 * when there is none.
	 *
	 * Both arguments come off the network. The id is only ever used to look an entity up in the tracker,
	 * never to create one: an avatar is minted solely for an id the tracker already holds, so an
	 * arbitrary packet cannot make this subsystem allocate anything. Function is the receiver the sender
	 * named, and the avatar is only returned when it actually is of that receiver's class.
	 *
	 * NULL IS THE ORDINARY ANSWER for an event declared on the class the entity's owner runs as a real
	 * actor, since no avatar is one of those. Such a call is not lost: the router offers it to
	 * HandleEntityEvent below, which applies whatever the game registered for the function. Nothing is
	 * charged against the entity's allowance on the way to that answer, so one inbound call is billed once,
	 * by whichever of the two paths actually delivers it.
	 *
	 * Returning null is a refusal rather than a reason to wait, which is why a spent allowance drops the
	 * event on the spot instead of queueing it.
	 */
	virtual UObject* ResolveReceiver(const FGuid& EntityUUID, const UFunction* Function) override;

	/**
	 * ICrowdyEntitySubscriber. Copies the values a decoded CrowdyState delta delivered into this entity's
	 * own fragments, driven entirely by DeliveredIndices: see the interface comment for why a layout slot
	 * outside that set must never be read.
	 */
	virtual void ApplyDecodedState(const FGuid& EntityUUID, const FCrowdyRepLayout& Layout,
		const void* DecodedContainer, TConstArrayView<int32> DeliveredIndices) override;

	/**
	 * The avatar for a tracked entity, minting one if needed. Null when the id names no live tracked
	 * entity, or when the configured avatar class cannot receive RequiredClass's events.
	 *
	 * RequiredClass is the class that declares the receiver being resolved; pass null when there is no
	 * such requirement. It is checked before anything is created, so a mismatched request costs nothing.
	 *
	 * Minting an avatar also enrolls it in the SDK entity registry as this entity's participant. The record
	 * is keyed by the ENTITY'S OWN id, never by anything derived from the avatar's class: every avatar in
	 * the crowd is the same class, so a class-derived id would collapse the whole crowd onto one record.
	 *
	 * The avatar handed back is the one this subsystem caches. A caller that keeps its own strong pointer
	 * to it does NOT keep it current: if the avatar is reclaimed, or released because the avatar class
	 * changed, the object stays alive in that caller's hand while events and the registry record move to
	 * a freshly minted one. Ask again rather than storing the pointer across frames.
	 */
	UCrowdyMassEntity* GetOrCreateAvatar(const FGuid& UUID, const UClass* RequiredClass);

	// Tells apart "no such entity", "never asked for", "here it is" and "reclaimed, ask again". See
	// ECrowdyMassAvatarState for why the last two are not the same answer.
	ECrowdyMassAvatarState GetAvatarState(const FGuid& UUID) const;

	// How many avatars this world will hold before it reclaims the least recently touched one: the
	// tracked-entity count, floored at CrowdyMassMinCachedAvatars. See that constant for why the capacity
	// follows the tracker instead of standing below it.
	int32 GetAvatarCacheCapacity() const;

	// Replaces the class new avatars are minted from. Every cached avatar is released, since the ones
	// already minted are of the old class and would keep answering for entities that should now have the
	// new one. A null class is refused and the current one is kept.
	void SetAvatarClass(TSubclassOf<UCrowdyMassEntity> InAvatarClass);

	/**
	 * The Mass entity manager this subsystem's entities live in, or null once the world is tearing down.
	 *
	 * Public because an avatar holds a bare entity handle and has to resolve it against the same manager
	 * this subsystem uses; giving it its own route to one would let the two disagree during teardown,
	 * which is the exact moment a handle is most likely to be stale.
	 */
	FMassEntityManager* GetMassEntityManager() const;

#if WITH_DEV_AUTOMATION_TESTS
	// Test seam: overrides the entity manager GetMassEntityManager() resolves, so a headless test can
	// register entities against a standalone manager it owns directly, with no World, MassEntitySubsystem,
	// spawner, or config asset in play.
	void SetMassEntityManagerForTest(FMassEntityManager* InManager) { MassEntityManagerForTest = InManager; }

	// Test seam: makes an id resolvable exactly as SpawnRemoteEntity's bookkeeping would, without going
	// through the Mass spawner or a config asset. The entity itself must already exist in whichever
	// manager SetMassEntityManagerForTest injected.
	void RegisterTrackedEntityForTest(const FGuid& UUID, FMassEntityHandle Entity)
	{
		UUIDToEntity.Add(UUID, Entity);
		EntityToUUID.Add(Entity, UUID);
	}

	// Test seam: registers CrowdyMass's own two standard view-state fields (Version and PoseId)
	// exactly as Initialize() does. A headless test built with
	// SetMassEntityManagerForTest above never calls Initialize() (there is no World subsystem
	// collection to construct this object through), so without this call ApplyViewState would resolve
	// against an empty field list and silently apply nothing.
	void RegisterStandardViewStateFieldsForTest() { RegisterStandardViewStateFields(); }

	// Test seam: registers CrowdyMass's own standard CrowdyState fragment field (the pose) exactly as
	// Initialize() does, for the same reason RegisterStandardViewStateFieldsForTest exists: a headless
	// test built with SetMassEntityManagerForTest never calls Initialize().
	void RegisterStandardStateFragmentFieldsForTest() { RegisterStandardStateFragmentFields(); }

	// Test seam: resolves one class's layout against the registered fields exactly as an arriving delta
	// does, so a case about what that resolution DECIDES needs no entity manager, no tracked entity and no
	// decoded buffer. Everything the decision is made of is the class, the layout and the registrations.
	void ResolveStateFragmentClassForTest(const UClass& Class, const FCrowdyRepLayout& Layout)
	{
		ResolveStateFragmentClass(Class, Layout);
	}

	// Test seam: injects the SDK entity registry a real world would supply, so a headless test can drive
	// avatar participant enrollment against a registry it owns, with no World, subsystem collection or
	// network in play.
	// Defined in the .cpp so this header keeps its forward declaration of the subsystem rather than including it.
	void SetEntitySubsystemForTest(UCrowdyEntitySubsystem* InEntities);

	// Test seam: forces the avatar cache capacity, so the reclaim path stays exercised. In a real world
	// the capacity follows the tracker and the cache cannot reach it, which is the point of the change
	// that made it follow the tracker; a fixture that could not lower it would leave the reclaim path,
	// and the registry cleanup that goes with it, with no coverage at all. Zero or negative restores the
	// real capacity.
	void SetAvatarCacheCapacityForTest(const int32 InCapacity) { AvatarCacheCapacityForTest = InCapacity; }

	// Shortens the reclaim grace window, so a test can drive both sides of it without waiting one out. Zero
	// makes every reclaim expire immediately, which is the behaviour the window replaced.
	void SetAvatarReclaimGraceSecondsForTest(const float InSeconds) { AvatarReclaimGraceSeconds = InSeconds; }

	// How many avatars are waiting out the grace window, so a test can tell "held for a redraw" from "released".
	int32 GetAvatarReclaimGraceCountForTest() const { return AvatarReclaimGrace.Num(); }

	// Runs the grace sweep as the armed timer would, so a headless fixture with no timer manager can prove the
	// window actually closes.
	void SweepExpiredAvatarGraceForTest() { SweepExpiredAvatarGrace(/*bForce*/ true); }

	// When the next sweep is due, so a test can prove it is the soonest deadline rather than a window from now.
	double GetNextAvatarGraceSweepSecondsForTest() const { return NextAvatarGraceSweepSeconds; }

	// Test seam: binds Config to EntityClass exactly as a resolved per-class binding on the rendering
	// backend would, and stops the backend override and the project-wide setting from answering for any
	// other class. The second half is not optional: a real project sets a Remote Entity Config, so without
	// it a case about a class nothing configures would be answered by whatever that project happens to
	// ship. A null Config binds nothing and only installs the suppression.
	// Defined in the .cpp so this header keeps its forward declaration of the config asset.
	void SetAuthoredEntityConfigForTest(const UClass* EntityClass, const UMassEntityConfigAsset* Config);

	// Test seam: the same drop a replaced entity config or a reloaded asset causes, reached directly so a
	// case can show that a cached MISS is dropped by it and not merely by whatever else the trigger did.
	void InvalidateAnimSetCachesForTest() { InvalidateAnimSetCaches(); }

	// Test seam: the config a class derives, built the same way a spawn of that class would build it. A
	// spawn needs a world and a Mass entity manager to reach this; the config itself needs neither, so this
	// is what lets a case ask what a class would be built from without standing a world up.
	UMassEntityConfigAsset* GetOrBuildDerivedEntityConfigForTest(const UClass* EntityClass)
	{
		return GetOrBuildDerivedEntityConfig(EntityClass);
	}

	// Test seam: the once-per-class report that a class's declared traits are shadowed by an authored config,
	// reached directly because the real caller needs a world and a built Mass template. The return value is
	// the whole point of the case: a Warning cannot be asserted, but "did this call say anything" can.
	bool ReportDeclaredTraitsNotConsumedForTest(const UClass* EntityClass, const UMassEntityConfigAsset* AuthoredConfig)
	{
		return ReportDeclaredTraitsNotConsumed(EntityClass, AuthoredConfig);
	}

#if WITH_EDITOR
	// Test seam: drives the live-edit hook with the object a details-panel edit would have named. A
	// headless case can neither install this subsystem in a world nor open a details panel, so what it can
	// still show is which objects the hook acts on, which is where the drop is either performed or missed.
	void HandleObjectPropertyChangedForTest(UObject* Object);

	// Test seam: drives the reinstancing hook with the map Live Coding would have handed it.
	void HandleObjectsReplacedForTest(const TMap<UObject*, UObject*>& ReplacementMap);

	// Test seam: reads back a per-class binding without going through the super-chain walk.
	const UMassEntityConfigAsset* FindExactEntityConfigBindingForTest(const UClass* EntityClass) const
	{
		const TObjectPtr<const UMassEntityConfigAsset>* Bound = EntityConfigByClass.Find(EntityClass);
		return Bound ? Bound->Get() : nullptr;
	}

	int32 NumEntityConfigBindingsForTest() const { return EntityConfigByClass.Num(); }
#endif

	// Test seams for the trace grid, which a headless case can drive with no world.
	bool GetSpatialGridCellsForSphereForTest(const FVector& Origin, const float Radius, const int32 MaxCells, TArray<FIntVector>& OutCells) const
	{
		return GetSpatialGridCellsForSphere(Origin, Radius, MaxCells, OutCells);
	}
	bool GetSpatialGridCellsForRayForTest(const FVector& Start, const FVector& End, const float QueryRadius, const int32 MaxCells, TArray<FIntVector>& OutCells) const
	{
		return GetSpatialGridCellsForRay(Start, End, QueryRadius, MaxCells, OutCells);
	}
	int32 NumRaySeenCellsForTest() const { return ScratchRaySeenCells.Num(); }
	void UpdateSpatialGridEntityForTest(const FMassEntityHandle Entity, const FVector& Origin, const float Radius)
	{
		UpdateSpatialGridEntity(Entity, Origin, Radius);
	}
	int32 NumSpatialGridCellsForTest(const FMassEntityHandle Entity) const
	{
		const FCrowdyMassSpatialGridEntry* Entry = SpatialGridEntries.Find(Entity);
		return Entry ? Entry->Cells.Num() : 0;
	}

	// Test seam: how many UUID-to-handle resolutions a path spent, so a case can hold the receive door to
	// one rather than asserting a timing.
	int32 GetEntityLookupCountForTest() const { return EntityLookupCountForTest; }
	void ResetEntityLookupCountForTest() { EntityLookupCountForTest = 0; }

	// Test seam: makes SpawnRemoteEntity's StateEntityClass bookkeeping inspectable directly, without a
	// live network delta, so a test can assert GetEntityClass answers what was stored.
	void SetStateEntityClassForTest(const FGuid& UUID, const UClass* StateEntityClass)
	{
		if (StateEntityClass)
		{
			// Through the production path, so the re-drive a class arriving triggers is exercised rather than
			// stepped around: this seam stands in for a spawn event naming the class.
			RecordStateEntityClass(UUID, StateEntityClass);
		}
		else
		{
			StateEntityClassByUUID.Remove(UUID);
		}
	}
#endif

#if !UE_BUILD_SHIPPING
	/**
	 * Dev seam: binds an animation set held in memory to one class, leaving every other class's binding
	 * alone.
	 *
	 * For the PIE gate harness, which has to prove the state plane's value-to-row mapping against rows it
	 * authored ITSELF. A gate reading whatever set a project happens to ship could not tell a mapping from
	 * an arriving byte copied straight through, because a project is free to author rows in the order that
	 * makes the two agree. It also keeps the gates' verdict a statement about this plugin rather than about
	 * a game's content, which is the same reason the state entity spawns from plugin defaults.
	 *
	 * Not the same as ConfigureOverrides, which rebuilds the whole binding set from ASSETS and would
	 * discard the bindings a backend resolved for every other class in a live session.
	 */
	void BindDevAnimSetForClass(const UClass* EntityClass, const UCrowdyMassAnimSet* AnimSet);
#endif

private:
	UPROPERTY()
	TObjectPtr<UMassEntitySubsystem> MassEntitySubsystem;

	UPROPERTY()
	TObjectPtr<UMassSpawnerSubsystem> MassSpawnerSubsystem;

	// This plugin's own remote entity config, composed in code rather than loaded, so a caller can spawn
	// an entity whose behaviour depends on nothing a project authored. Built on first use and held here
	// because it owns the trait objects it was composed from, which the template it produces is rebuilt
	// from whenever the world asks for one.
	UPROPERTY()
	TObjectPtr<UMassEntityConfigAsset> PluginDefaultRemoteEntityConfig;

	TMap<FGuid, FMassEntityHandle> UUIDToEntity;
	TMap<FMassEntityHandle, FGuid> EntityToUUID;
	TMap<FIntVector, TSet<FMassEntityHandle>> SpatialGridCells;
	TMap<FMassEntityHandle, FCrowdyMassSpatialGridEntry> SpatialGridEntries;
	TMap<FMassEntityHandle, FCrowdyMassTraceInstanceCacheEntry> TraceInstanceCache;

	/**
	 * Working memory for the spatial grid queries below, kept across calls instead of built on the stack
	 * per query.
	 *
	 * A turret attack runs two full traces, and a query that fills a set with a few thousand entities from
	 * empty rehashes it about a dozen times on the way, allocating and freeing at every step. Reusing the
	 * storage makes that cost land once per world rather than once per trace. Each is Reset, never
	 * Emptied, which is what keeps the storage; none is reserved from the crowd size, because a query that
	 * touches a handful would then pay for the whole crowd every time.
	 *
	 * Mutable because holding on to scratch memory changes nothing a caller can observe, and every query
	 * here is const.
	 *
	 * These are NOT re-entrant. Each is reset by the query that owns it, so a caller that reached one of
	 * these queries from inside another (a visitor callback that traces again, for instance) would have
	 * the outer query's working set emptied underneath it. Nothing does that today; keep it that way.
	 */
	mutable TArray<FIntVector> ScratchQueryCells;
	mutable TSet<FIntVector> ScratchRaySeenCells;
	mutable TSet<FMassEntityHandle> ScratchUniqueEntities;
	mutable TSet<FIntVector> ScratchTraversalSeenCells;
	mutable TArray<FCrowdyMassTraceCandidateCell> ScratchTraversalCells;
	mutable TArray<FCrowdyMassTraceCandidateCell> ScratchSortedCandidateCells;

	// Handed out by ResetTraceTestedEntitiesScratch; see that function for why the trace path's working
	// set is owned here and not by the trace library.
	TSet<FMassEntityHandle> ScratchTraceTestedEntities;

	// The hit zone table this world's trace results are resolved against, already flattened. Held here
	// rather than in the trace library so that two PIE sessions, and the two worlds alive during level
	// travel, cannot decide each other's hit zones.
	FCrowdyMassResolvedHitZoneTable ResolvedHitZoneTable;

	// Registered view-state fields, in registration order. CrowdyMass's own two standard fields are
	// always indices 0 and 1, added by Initialize() before a game gets the chance to register its own.
	//
	// Held indirectly so that a registration's storage never moves. RegisterViewStateField is public
	// and an ingestor is one call away from it, directly or through anything it broadcasts to, so a
	// field registered while the apply loop is running must not be able to move the ingestor that is
	// currently executing. Registration only ever appends, so an index taken before an ingestor ran
	// still names the same field after it.
	TArray<TUniquePtr<FCrowdyMassViewStateFieldRegistration>> ViewStateFieldRegistrations;

	// Property offsets resolved once per distinct wire UScriptStruct and reused for every later update
	// of that type, so applying the echo costs a pointer-offset read per registered field rather than a
	// name search. Assumes a UScriptStruct's property layout never changes for the life of the process:
	// native struct layout is fixed at compile time, and unlike a Blueprint class, Live Coding does not
	// re-instance a USTRUCT's properties, so there is nothing here that needs invalidating.
	//
	// Shared rather than stored by value because RegisterViewStateField empties this whole map, and an
	// ingestor can reach that call. The apply loop keeps its own reference to the entry it is walking,
	// so a registration arriving mid-update cannot free the field list being read. That costs one
	// non-atomic reference count per update and no allocation; see the alias above for why the count
	// is not the default atomic one.
	TMap<const UScriptStruct*, FCrowdyMassViewStateResolvedStructRef> ViewStateResolvedStructs;

	// Wall-clock time the next "view-state echo carries an unrecognised wire version" warning may be
	// logged, so a peer stuck on a newer version does not scroll the log every update.
	double NextUnknownViewStateVersionWarningSeconds = 0.0;

	// Per-entity class used for routing on the ICrowdyEntitySubscriber path, set from the same
	// locally-resolved class UCrowdyActorManager::ResolveEntityClass already produces for this entity
	// (threaded in through SpawnRemoteEntity's StateEntityClass parameter) and cleared in UnregisterEntity.
	// Never populated from anything off the wire: GetEntityClass's answer is the router's security
	// boundary, so what it returns must trace back to authored data this client resolved itself, exactly
	// like the participant path's ClassID.
	//
	// Sparse on purpose: an entity is tracked whether or not it has an entry here, so its absence says the
	// entity has no class, never that the entity is unknown.
	TMap<FGuid, TWeakObjectPtr<const UClass>> StateEntityClassByUUID;

	// Registered CrowdyState fragment fields, in registration order. Mirrors ViewStateFieldRegistrations;
	// see FCrowdyMassStateFieldRegistration. CrowdyMass's own standard pose field is always index 0, added
	// by Initialize() before a game gets the chance to register its own.
	TArray<TUniquePtr<FCrowdyMassStateFieldRegistration>> StateFieldRegistrations;

	// One class's resolution of StateFieldRegistrations against its FCrowdyRepLayout, parallel to
	// ViewStateResolvedStructs; see FCrowdyMassStateResolvedClass. Keyed by class rather than by
	// UScriptStruct because a CrowdyState layout belongs to the entity's own class, not to a shared wire
	// struct several classes might send.
	//
	// The key is a RAW class pointer while its siblings hold an FObjectKey, and that is only safe because of
	// something outside this map. ResolveStateFragmentClass resolves the class's animation set before it adds
	// the entry, and doing so puts the same class into AnimSetByClass, which is a UPROPERTY holding it
	// strongly. AnimSetByClass has no Remove, and its only Empty sits in InvalidateAnimSetCaches beside this
	// map's own, so every key here is kept alive for exactly as long as it is a key. A collected class whose
	// address was recycled therefore cannot produce a hit under the wrong class.
	//
	// The consequence is a constraint on that other map: adding a targeted per-class Remove to AnimSetByClass
	// would take the anchor away and let an address be recycled into a wrong-class hit on the
	// network-arrival path. Convert this key to FObjectKey first if that is ever wanted.
	TMap<const UClass*, FCrowdyMassStateResolvedClassRef> StateResolvedClasses;

	// Classes already reported as carrying no property for a field this build considers standard, so the
	// message is said once per class rather than once per resolution.
	//
	// Deliberately NOT a time gate. The resolution it accompanies is cached per class, so a class is only
	// asked about again after an invalidation: a time gate silently drops the message for every class but
	// the first in a level-load burst, and never says it again for the life of the world. Emptied wherever
	// the answer can change, so the remedy a message asks for is confirmed by the message stopping.
	TSet<const UClass*> StateFieldAbsentReportedClasses;

	// Every (class, property) currently decided to reach no registered fragment field. Doubles as the memo
	// that keeps the warning to one line per property and as the record crowdy.mass.state.verdict prints,
	// so the two can never disagree about what was suppressed: an entry exists exactly when a line was
	// written for it, and its Detections figure is how many decisions that one line now stands for.
	//
	// Deliberately NOT emptied beside StateFieldAbsentReportedClasses, which is what makes the message once
	// per world rather than once per invalidation: the caches around it are dropped whenever an animation
	// set, a class or a registration moves, and a memo emptied with them would repeat every line each time.
	// Kept accurate instead by the resolution itself, which REMOVES a property that has come to reach a
	// field, so the remedy a line asks for is confirmed by that property leaving this record.
	TMap<FCrowdyMassClassPropertyKey, FCrowdyMassDroppedStateProperty> DroppedStateProperties;

	// Function -> every handler registered for it, in registration order. This is all the resolution this
	// plane needs: a call arrives already naming its function. Keyed by FObjectKey rather than by a raw
	// pointer so a collected function's address cannot be recycled into a stale hit that would run the
	// wrong handler, and each registration also holds the function weakly so the hit can be confirmed.
	//
	// Held indirectly, and by shared reference rather than by value, for the same reason as every other
	// registration surface here: register and unregister are public and a handler is one call away from
	// both, so neither may move or destroy the handler currently executing.
	//
	// CrowdyMass registers nothing of its own here. Which events a crowd entity reacts to is the consuming
	// game's decision, declared on the game's own actor class; a standard entry would be this plugin
	// inventing a game's noun.
	TMap<FObjectKey, TArray<FCrowdyMassEventHandlerRef>> EventHandlersByFunction;

	// Source of the ids handed out as FCrowdyMassEventHandlerHandle. Starts at one so a default-constructed
	// handle can never name a real registration.
	uint64 NextEventHandlerId = 1;

	// Events already reported as declared actions that carry no action id. Said once per function rather
	// than once per inbound event, because the cause is an authoring mistake that lasts as long as the
	// declaration does and every entity receiving the event would otherwise repeat it.
	TSet<FObjectKey> ReportedActionEventsMissingId;

	// Events that arrived, ran their body, and started no action because nothing in this process says they
	// are one. Said once each: whether a marker reached this process is a fact about the process.
	TSet<FObjectKey> ReportedEventsNotDeclaredActions;

	// Wall-clock time the next "an event was aimed at an entity of a different class" warning may be logged.
	// The cause lasts as long as the mistake does and repeats per entity per event, so it is rate-gated
	// rather than said once: a game that fixes it and re-aims the event needs to hear about the next one.
	double NextEventClassRefusalWarningSeconds = 0.0;

	// The animation set each entity class renders through, worked out on first use and kept. Held rather
	// than looked up on demand because finding one means loading two soft references, which is not
	// something a send path or a Blueprint tick can afford to do on the frame of the first death.
	//
	// Keyed by class because a set is per class: two classes with different sets is the whole point, and
	// one answer for the whole backend would hand every class the first one's rows. A MISS is kept exactly
	// like a hit, and carries WHICH miss it was, so a class that names nothing is not walked again for
	// every entity of it that ever appears.
	//
	// Mutable because working an answer out changes nothing a caller can observe, which is what lets the
	// accessors stay const for a diagnostic holding this subsystem by const pointer.
	UPROPERTY(Transient)
	mutable TMap<TObjectPtr<const UClass>, FCrowdyMassClassAnimSet> AnimSetByClass;

	// Authored pose names resolved to ids, per class, including the names that resolve to nothing.
	// Mutable for the same reason as the map above, and that is what lets FindPoseId stay callable from a
	// send path without either its callers or its Blueprint signature having to pretend otherwise.
	//
	// Every entry is derived from a class's entry in AnimSetByClass, so both are emptied together and
	// neither outlives the asset it describes. Both directions matter: a replaced config answers for a
	// different set of rows, and a MISS is cached exactly like a hit, so a name that resolved to nothing
	// before a row was added for it would keep resolving to nothing for the life of the world.
	mutable TMap<FCrowdyMassClassPoseKey, int32> PoseIdsByClassAndName;

	// Wall-clock time the next "a pose was asked for with no entity class" warning may be logged. Rate
	// gated rather than said once, because the cause is a caller that lost the class and repeats every
	// time it runs, and a game that fixes one call site needs to hear about the next.
	mutable double NextAnimSetNoClassWarningSeconds = 0.0;

	// Avatars minted so far, bounded by GetAvatarCacheCapacity. A UPROPERTY map because these are the
	// only references keeping the avatars alive.
	UPROPERTY()
	TMap<FGuid, FCrowdyMassAvatarEntry> Avatars;

	// Null means the default avatar class; GetEffectiveAvatarClass resolves it. Kept as a class rather
	// than resolved once at startup so a game can substitute its own subclass before any entity spawns.
	UPROPERTY()
	TSubclassOf<UCrowdyMassEntity> AvatarClass;

	// Ids whose avatar was reclaimed while the entity is still live. Bounded by the tracked entity count,
	// because an id only lands here after having an avatar, which requires the tracker to hold it, and it
	// is removed again when that entity is unregistered.
	TSet<FGuid> EvictedAvatarUUIDs;

	// Avatars whose entity has left, still holding their registry records until the grace window runs out.
	// Bounded by the crowd that has been drawn: an entry is only ever made for an id that held an avatar.
	UPROPERTY()
	TMap<FGuid, FCrowdyMassAvatarGraceEntry> AvatarReclaimGrace;

	// Grace length in seconds, so a test can shorten it. Never changed at runtime.
	float AvatarReclaimGraceSeconds = CrowdyMassAvatarReclaimGraceSeconds;

	// When the next whole-map sweep of the grace window may run, on FPlatformTime::Seconds. An entry that
	// is asked for is expired exactly, at the moment it is asked for; this only reclaims the ones nobody
	// comes back for, and it walks a map bounded by the crowd at most once per window.
	//
	// Set to the soonest surviving deadline rather than a window ahead, so a sweep is never refused while
	// something is already overdue.
	double NextAvatarGraceSweepSeconds = 0.0;

	// The one armed sweep, aimed at the soonest deadline. Without it a row that leaves and never returns keeps
	// its registry record for the rest of the session, because every other route into the sweep needs some
	// other row to leave.
	FTimerHandle AvatarGraceSweepTimer;

	/**
	 * Inbound allowance per entity. Bounded by the tracker: an entry is only ever created for an id the
	 * tracker already holds, so an inbound flood of unknown ids allocates nothing here, and
	 * ReleaseAvatarState drops the entry when the entity leaves.
	 *
	 * Deliberately NOT cleared when an avatar is reclaimed, even though the avatar for the same id is.
	 * The bucket describes the entity, not the avatar, and reclaiming is driven by pressure on the cache,
	 * which a flood is one way to produce. Clearing here would hand a full fresh burst back to whatever
	 * caused the reclaim, which is the one case this exists to survive.
	 */
	TMap<FGuid, FCrowdyMassReceiveBudget> ReceiveBudgets;

	int64 AvatarTouchCounter = 0;

	// How many entities have been refused trace collision here for carrying no trace collision fragment,
	// and whether the warning naming the missing trait has been said yet. Once, because a whole crowd
	// built from one config is missing it together, so a line per entity would be thousands of copies of
	// one sentence.
	int32 MissingTraceCollisionFragmentCount = 0;
	bool bWarnedMissingTraceCollisionFragment = false;

	// An avatar class that cannot receive a declared event is a configuration mistake, so it says the
	// same thing every time that event fires. Reported once, loudly, rather than once per inbound event.
	bool bWarnedAvatarClassMismatch = false;

	// Every eviction is logged, because a cache that silently shrinks looks exactly like a cache that is
	// working. The per-eviction line sits at Verbose and the rate-gated summary at Warning: thrashing
	// evicts once per mint, and a warning each would hand a flood an amplifier, while a summary carrying
	// the count since the previous line cannot be missed at default verbosity.
	int32 AvatarEvictionCount = 0;
	int32 AvatarEvictionsSinceLog = 0;
	double LastAvatarEvictionLogSeconds = 0.0;

	// The router this subsystem registered itself as an entity subscriber with. Weak because world
	// subsystem teardown order is not guaranteed, so the router may already be gone when this one
	// deinitializes.
	TWeakObjectPtr<UCrowdyEventRouter> EventRouter;

public:

	/**
	 * Writes one self-contained verdict block to the log for the participant-less CrowdyState path.
	 *
	 * It exists because the two roads a pose can travel write the same byte with the same value while the
	 * echo is still live, so a corpse looking right proves nothing about whether the state plane works. The
	 * block reports which road actually wrote, how many deltas were claimed and applied, and every drop
	 * reason separately, because "nothing arrived" and "everything arrived and was refused" look identical
	 * from outside and call for opposite investigations.
	 *
	 * It opens with every replicated property no registered fragment field claims, which is the one part
	 * that is a fact about authoring rather than about traffic: those values arrive intact and are thrown
	 * away, and each is warned about only once, so this is where the whole list can be read back.
	 */
	void LogStateVerdict() const;

	// Poses written since the counters were last reset. Diagnostic only; nothing branches on them.
	//
	// There used to be a second counter beside this one for the view-state echo, because the pose had two
	// transports and no amount of looking at a corpse could say which one delivered it. The echo's pose
	// field was deleted in Phase A.4, so there is one road and the attribution question no longer exists.
	int64 PoseWritesFromState = 0;

	// A pose write carrying 0 is the "no pose" sentinel, and an entity that is simply alive sends it on
	// every heartbeat, so the count above rises steadily whether or not a real pose has ever crossed. This
	// records only the writes that carried an actual pose, which is what says a VALUE arrived rather than
	// the road merely being busy.
	//
	// The two are read as a PAIR across successive readings, and that is what makes a clear provable
	// without judging a picture: this count rising says a pose arrived, and this count HOLDING while the
	// one above keeps climbing says the pose was cleared, since a heartbeat re-sends whatever the stance
	// currently is. The last value is kept so one line can say which pose it was.
	int64 NonZeroPoseWritesFromState = 0;
	uint8 LastNonZeroPoseFromState = 0;

private:

#if WITH_EDITOR
	FDelegateHandle ObjectsReplacedHandle;
	FDelegateHandle ObjectPropertyChangedHandle;
#endif

	// Per-backend overrides; fall back to UCrowdyMassDeveloperSettings when unset/negative.
	TSoftObjectPtr<UMassEntityConfigAsset> RemoteEntityConfigOverride;
	float SmoothingDurationOverride = -1.0f;

	// The flattened per-class bindings from the backend config, resolved once in ConfigureOverrides.
	// Both sides are hard pointers because the assets they name were loaded to build this map and are
	// kept alive by it, which is what makes a spawn one lookup and no load.
	UPROPERTY(Transient)
	TMap<TObjectPtr<const UClass>, TObjectPtr<const UMassEntityConfigAsset>> EntityConfigByClass;

	// Configs built from a class rather than authored, for classes nothing has a config for. Kept per
	// class because deriving reads a class default object and every Blueprint in its parent chain, which
	// is far too much to repeat per spawn; the answer cannot change while the class is the same object,
	// and the cases where a class is replaced underneath us are the same ones that empty every other
	// cache here.
	UPROPERTY(Transient)
	TMap<TObjectPtr<const UClass>, TObjectPtr<UMassEntityConfigAsset>> DerivedConfigByClass;

	// An authored config with the class's own near tier shell declaration wired into it, per class. Kept
	// for the same reason the derived configs above are: answering costs a class default object walk down
	// a parent chain and a synchronous class load, and the answer cannot change while the class, the shell
	// and the authored config are all the same objects. The last of those three is held in the entry and
	// compared on every lookup, so a backend swap that moves which config answers for a class cannot be
	// served a patch built over the previous one.
	UPROPERTY(Transient)
	TMap<TObjectPtr<const UClass>, FCrowdyMassNearTierPatchedConfig> NearTierPatchedConfigByClass;

	// Classes already told that the shell they declare is not what promotes them. Said once per class: the
	// cause is authoring that lasts as long as the config does, and a crowd would otherwise repeat it for
	// every entity that ever appears, which buries the one line that says what to remove.
	TSet<TObjectPtr<const UClass>> ReportedNearTierShellNotConsumedClasses;

	// Classes already asked whether the Mass traits they declare on themselves are shadowed by an authored
	// config. Every class ASKED is recorded, not only every class told, because answering costs a class
	// default object walk down a Blueprint parent chain and a class that declares nothing is the common
	// case: recording only the ones that had something to say would repeat that walk for every entity of
	// every configured class that ever appears. It doubles as the throttle, so the one line naming what to
	// move is said once rather than buried by a crowd.
	TSet<TObjectPtr<const UClass>> CheckedDeclaredTraitsAgainstAuthoredConfigClasses;

	// The per-class stand-ins an inbound event's own body runs on. Held strongly because nothing else
	// references a proxy and a collection between events would leave the memo naming an object that has
	// gone. Keyed and invalidated exactly like DerivedConfigByClass above, since a recompiled Blueprint
	// mints a new class and the proxy of the old one stands for nothing.
	UPROPERTY(Transient)
	TMap<TObjectPtr<const UClass>, FCrowdyMassDispatchProxyEntry> DispatchProxyByClass;

	// The proxy plus its cached reset list. GetOrCreateDispatchProxy hands back only the proxy, which is all
	// a test wants; the dispatch path needs both.
	FCrowdyMassDispatchProxyEntry* GetOrCreateDispatchProxyEntry(const UClass* EntityClass);

	// Classes already reported as having no proxy that can be built. A class that cannot supply one this
	// event cannot supply one on the next either, and a crowd would otherwise say so once per entity per
	// event for as long as it lasts.
	TSet<TObjectPtr<const UClass>> ReportedUnproxyableClasses;

	// Classes that DECLARE an event whose body a stand-in cannot run, already reported. An event declared on
	// a component of the entity's class is allowed to be aimed at that entity, because an observer holding it
	// as a real actor resolves the body onto the component; a stand-in is constructed rather than spawned and
	// carries no such component, so the call is refused rather than run against an object of the wrong class.
	// Said once per declaration, since it cannot be hosted on the next event either. Written only through
	// ReportUnhostableEventDeclaration, so the allowance step and the dispatch share one throttle.
	//
	// Keyed by FObjectKey rather than by the class pointer, so a function with no owner class at all still
	// gets an entry of its own rather than falling through the throttle unreported, and so a collected class
	// cannot be recycled into a stale hit that would silence the next declaration.
	TSet<FObjectKey> ReportedUnhostableEventClasses;

	// Whether this world has already said it will not run a body on a stand-in. A property of the world, so
	// it is true for every event for the whole session once it is true at all, and saying so per event would
	// bury it.
	bool bReportedProxyWorldWillNotRunBodies = false;

	// Classes already reported as underivable. A class that cannot supply a mesh cannot supply one on the
	// next spawn either, so without this the warning arrives once per entity for as long as the crowd
	// lasts, which buries the one line that says what to fix.
	TSet<TObjectPtr<const UClass>> ReportedUnderivableClasses;

#if WITH_DEV_AUTOMATION_TESTS
	// Set only by SetMassEntityManagerForTest; GetMassEntityManager() prefers this over the real
	// MassEntitySubsystem when present.
	FMassEntityManager* MassEntityManagerForTest = nullptr;

	// Set only by SetEntitySubsystemForTest; the resolver prefers it over the world's own subsystem when
	// present. Weak so a fixture that tears its registry down first leaves nothing here pointing at freed
	// memory.
	TWeakObjectPtr<UCrowdyEntitySubsystem> EntitySubsystemForTest;

	// Set only by SetAvatarCacheCapacityForTest; GetAvatarCacheCapacity prefers it when positive.
	int32 AvatarCacheCapacityForTest = 0;

	// Set only by SetAuthoredEntityConfigForTest; makes GetRemoteEntityConfigAsset stop after the
	// per-class bindings instead of falling through to the backend override and the project setting.
	bool bSuppressUnboundEntityConfigForTest = false;

	// Counted in FindEntityByUUID. Mutable because that function is const and counting changes no answer.
	mutable int32 EntityLookupCountForTest = 0;
#endif

	bool ApplyRemoteEntityTransform(const FGuid& UUID, const FVector& Location, const FQuat& Rotation, int64 ServerTimestampMs, float SmoothingDuration);
	// The body of the above, for callers that already hold a resolved and validated handle.
	bool ApplyRemoteEntityTransformToEntity(FMassEntityManager& EntityManager, FMassEntityHandle Entity, const FVector& Location, const FQuat& Rotation, int64 ServerTimestampMs, float SmoothingDuration);
	bool InitializeCrowdyFragments(FMassEntityManager& EntityManager, FMassEntityHandle Entity, const FGuid& UUID, const FTransform& InitialTransform) const;
	void UnregisterEntity(const FGuid& UUID, const FMassEntityHandle Entity);
	// EntityClass may be null, which is the pre-per-class behaviour: fall straight through to the
	// backend override and then the project-wide setting. OutSource, when given, names the branch that
	// answered, so a caller can report which one won without re-deriving the ordering, and separates an
	// exact binding from one inherited from an ancestor class.
	const UMassEntityConfigAsset* GetRemoteEntityConfigAsset(const UClass* EntityClass,
		ECrowdyMassEntityConfigSource* OutSource = nullptr) const;

	// The binding entry answering for EntityClass, and through OutBoundClass the class carrying it. The
	// one walk of the super chain; FindEntityConfigBindingClass is this with the entry discarded, so the
	// two can never disagree about which class answers.
	const TObjectPtr<const UMassEntityConfigAsset>* FindEntityConfigBinding(const UClass* EntityClass,
		const UClass*& OutBoundClass) const;

	// A config built from what the class itself carries, for a class nothing has authored one for. This is
	// the LAST resort and never overrides anything: an authored config, a backend override and the
	// project-wide setting all still win, in that order, so adding this changes nothing for a project that
	// already configures its crowd. Returns null, having reported once for that class, when the class
	// supplies no mesh to build from.
	//
	// It also carries an animation trait when the class names an animation set on a Crowdy Mass Anim Set
	// component, and the mesh description takes that set's Bank as its transform provider, which together
	// make such a class animate rather than stand in its bind pose. A class that names no set, or a set
	// with no Bank, still builds a config and still appears; it simply does not move, and says so once.
	//
	// bAuthoredConfigForcedAside changes only what is reported: it says an authored config existed and
	// crowdy.mass.derive.force pushed it aside, which is the difference between this path being the last
	// resort and it being the thing under test.
	UMassEntityConfigAsset* GetOrBuildDerivedEntityConfig(const UClass* EntityClass, bool bAuthoredConfigForcedAside = false);

	void InvalidateDerivedConfigs();

	// Says once, for a class an authored config answers for, that the Mass traits it declares on its own
	// Crowdy Mass Entity Traits component are not read. Reports only; it never changes what is used.
	//
	// The rule it reports is that a config's trait list is the whole statement of what its entities are made
	// of. Adding to it from a component would give a project traits its config never lists, removable only by
	// finding a component on a class the config does not mention, so the config wins outright. Said rather
	// than done quietly, because a list somebody filled in that has no effect has no other symptom.
	//
	// Returns whether this call said anything, which is false both for a class already asked and for a class
	// with nothing to say.
	bool ReportDeclaredTraitsNotConsumed(const UClass* EntityClass, const UMassEntityConfigAsset* AuthoredConfig);

	// AuthoredConfig with the class's own near tier shell declaration wired into it, or AuthoredConfig
	// itself when there is nothing to wire. Never null for a non-null AuthoredConfig, so a caller reaches
	// for it in place of the config it already has rather than around it.
	//
	// This exists because promotion is declared on the CLASS and used to be read only where a config is
	// built FROM a class, which is the path taken exactly when nothing authored answers. A class with an
	// authored config therefore declared a shell that nothing read, silently, and the opt-in was half true.
	//
	// It never overrides an authored decision. A config that already promotes on its own keeps doing
	// exactly what it did, and the declaration it shadows is reported once rather than quietly ignored.
	//
	// World is taken because Mass filters traits by the execution flags it derives from the world, so which
	// trait a template is actually built from is a property of the world and not of the config alone. A
	// patch chosen under a different rule than the template is built under is a patch that changes nothing
	// and says it worked.
	const UMassEntityConfigAsset* GetOrBuildNearTierPatchedConfig(const UWorld& World, const UClass* EntityClass,
		const UMassEntityConfigAsset* AuthoredConfig);

	// The Mass template a remote entity is spawned from. Names in OutWhy what was missing when it
	// answers null, because every caller reports that to somebody rather than retrying.
	const FMassEntityTemplate* ResolveRemoteEntityTemplate(const UWorld& World, ECrowdyMassRemoteEntitySource Source,
		const UClass* EntityClass, FString& OutWhy);

	// This plugin's own remote-entity template, composed in code from this plugin's own traits so it
	// carries exactly what those traits produce rather than a hand-copied list that could drift from
	// them. It deliberately carries no visualization: nothing spawned this way is drawn, because
	// drawing needs a mesh and this plugin ships no content.
	const FMassEntityTemplate* GetOrBuildPluginDefaultTemplate(const UWorld& World);

	// Drops every class's animation set answer and every pose id derived from one, so both are worked out
	// again on next use. Called wherever ANY class's set can have changed: a replaced entity config
	// answers with different rows, and a reloaded asset re-authors the rows in place.
	//
	// The misses go too, and that is the half that is easy to leave out: a class that resolved to nothing,
	// or a name that resolved to no row, would otherwise keep answering nothing for the life of the world
	// even after the very thing a warning asked for was authored.
	void InvalidateAnimSetCaches();

	// Drops every class's near tier answer for the authored config path, including the classes already
	// told their declaration is not what promotes them. Both halves matter: an author acting on one of
	// those warnings has to see the result in the session that printed it, and a class still shadowing
	// its config has to be able to say so again.
	void InvalidateNearTierPatchedConfigs();

	// Works out which animation set EntityClass renders through and why, loading the config and the set to
	// find out, and says once per class which source supplied it. Only FindRemoteEntityAnimSet calls this;
	// everything else reads the cached answer.
	const UCrowdyMassAnimSet* LoadRemoteEntityAnimSet(const UClass* EntityClass,
		ECrowdyMassAnimSetSource& OutSource) const;

	// Whether the next "a pose was asked for with no entity class" warning may be logged now, advancing
	// the gate when it may.
	bool ShouldWarnAnimSetNoClass() const;

	float GetDefaultSmoothingDuration() const;

	// Registers CrowdyMass's own two standard view-state fields through the same RegisterViewStateField
	// call a consuming game uses for its own, so neither is special-cased ahead of the mechanism.
	void RegisterStandardViewStateFields();

	// Applies every registered view-state field found on State's script struct directly, given an
	// entity manager and entity the caller already has in hand. ApplyViewState(UUID, State) is the
	// public UUID-keyed wrapper; SpawnRemoteEntity calls this directly to avoid a redundant UUID lookup
	// when it already holds both.
	void ApplyViewStateToEntity(const FInstancedStruct& State, FMassEntityManager& EntityManager, FMassEntityHandle Entity);

	// Resolves (and caches) every registered field's FProperty against one wire UScriptStruct. The
	// caller keeps the returned reference for as long as it reads the field list, which is what lets a
	// field registered from inside an ingestor empty the cache without pulling that list out from under
	// the update in progress.
	FCrowdyMassViewStateResolvedStructConstRef ResolveViewStateStruct(const UScriptStruct& ScriptStruct);

	// Registers CrowdyMass's own standard CrowdyState fragment field (the pose) through the same
	// RegisterStateFragmentField mechanism a consuming game uses for its own, so neither is
	// special-cased ahead of it. Called from Initialize() alongside RegisterStandardViewStateFields().
	void RegisterStandardStateFragmentFields();

	// Shared body for RegisterStateFragmentField and RegisterStandardStateFragmentFields; bIsStandard
	// marks a field whose absence from a class's layout is worth the diagnostic in ResolveStateFragmentClass
	// (see FCrowdyMassStateFieldRegistration::bIsStandard).
	void RegisterStateFragmentFieldInternal(FName PropertyName, ECrowdyMassStateFieldType FieldType,
		FCrowdyMassViewStateIngestor Ingestor, bool bIsStandard,
		ECrowdyMassStateFieldMatch Match = ECrowdyMassStateFieldMatch::ByName);

	/**
	 * The pose id an arriving state value means for this class, or 0 when it means no pose.
	 *
	 * The mapping belongs to the class's own animation set, which is what makes the same value mean two
	 * different clips on two classes rather than colliding. Done here, once per arriving delta, so the
	 * fragment keeps holding a row index and the per-entity animation loop stays one bounds check and one
	 * array index.
	 */
	int32 MapPoseStateValueToPoseId(const UClass* EntityClass, int64 StateValue) const;

	/**
	 * The enum a class's animation set declares its pose rows to be values of, and where the answer came
	 * from, or null when the class has no set or its set names no enum.
	 *
	 * OutSource distinguishes the ways this can answer nothing, which are different faults with different
	 * remedies: no class at all, a class declaring no set, a set that failed to load, and a set that
	 * loaded but names no enum. Collapsing them into one branch would send a reader to the wrong place.
	 */
	const UEnum* FindPoseStateEnum(const UClass* EntityClass, ECrowdyMassAnimSetSource& OutSource) const;

	// Resolves (and caches) every registered CrowdyState fragment field's layout slot against one class's
	// FCrowdyRepLayout, exactly as ResolveViewStateStruct does for a wire UScriptStruct. Rebuilds when the
	// cached entry's LayoutHash has moved under it, matching UCrowdyEventRouter::ResolveStateScratch's own
	// rebuild-on-mismatch rule for the same buffer's layout.
	FCrowdyMassStateResolvedClassConstRef ResolveStateFragmentClass(const UClass& Class, const FCrowdyRepLayout& Layout);

	// Writes the section of the verdict block listing every CrowdyState property no registered fragment
	// field claims. Split out because it is the one part of that block made of resolution facts rather
	// than of router counters, so it stays worth printing when there is no router left to ask.
	void LogDroppedStatePropertyVerdict() const;

	UClass* GetEffectiveAvatarClass() const;

	/**
	 * Whether an avatar for this entity could receive a function declared on RequiredClass, asked before
	 * anything is minted or charged.
	 *
	 * It answers the question GetOrCreateAvatar answers by refusing, and it has to mirror that function's
	 * two branches exactly: a cached avatar still standing for this entity is judged on its own class, and
	 * anything else on the class a fresh mint would use. Answering from only one of them would refuse a
	 * call an avatar could have taken, or charge for one it could not.
	 *
	 * A null RequiredClass is no requirement at all, which is true.
	 */
	bool CanAvatarReceive(const FGuid& UUID, FMassEntityHandle Entity, const UClass* RequiredClass) const;

	// The SDK entity registry for this world, or null when there is none: the transient world the engine
	// builds at startup has none, and neither does a headless fixture. Every call through it tolerates the
	// absence, because an avatar with no record still receives events.
	UCrowdyEntitySubsystem* ResolveEntitySubsystem() const;

	// Enrolls Avatar as this entity's participant in the SDK entity registry. Called when an avatar is
	// minted, and only then.
	void RegisterAvatarParticipant(const FGuid& UUID, UCrowdyMassEntity* Avatar);

	// The one place a class is recorded for an id. A caller with no class to give leaves whatever is
	// recorded alone; see SpawnRemoteEntity for why.
	void RecordStateEntityClass(const FGuid& UUID, const UClass* Class);

	// Drops that record, but only while it still names this exact avatar: see the definition for why
	// removing it by id alone would unbind whatever registered under the id afterwards.
	void UnregisterAvatarParticipant(const FGuid& UUID, const UCrowdyMassEntity* Avatar);

	// The same for every cached avatar at once, for the paths that drop the whole cache.
	void UnregisterAllAvatarParticipants();

	// Drops the least recently touched avatar, with the entity registry record it enrolled, and marks the
	// id as reclaimed. False when there was nothing to drop, which is what stops the caller looping.
	bool EvictOldestAvatar();

	// Forgets everything held for one entity: its avatar and its evicted mark, and its inbound allowance,
	// which only this path clears. Called when the entity leaves the tracker, so none of these outlive
	// the entity they describe.
	//
	// A live avatar is moved into the grace window rather than dropped, keeping its registry record, so an
	// entity drawn again shortly afterwards is not enrolled afresh.
	void ReleaseAvatarState(const FGuid& UUID);

	// The avatar held in the grace window for this entity, re-bound to the handle it is drawn as now and
	// moved back into the cache, or null when nothing is held or the window has run out. An expired entry
	// is unregistered here, so the caller mints afresh exactly as it would for an entity never drawn.
	UCrowdyMassEntity* ReclaimAvatarFromGrace(const FGuid& UUID, FMassEntityHandle Entity);

	// Drops every grace entry whose window has run out. Force skips the interval gate, which is what the armed
	// timer passes: the paths that create and consult entries only run while the crowd is churning, and a row
	// that leaves and never comes back is exactly the one nothing would run for.
	void SweepExpiredAvatarGrace(bool bForce = false);

	// Aims the sweep timer at DeadlineSeconds, so a window closes when it says it does. A deadline no sooner than
	// the one already armed rides that timer instead of replacing it, which keeps a bulk departure linear.
	// A worldless subsystem has no timer manager and keeps the caller-driven behaviour alone.
	void ArmAvatarGraceSweep(double DeadlineSeconds);

	// The deadline the sweep timer is currently aimed at, so a departure can tell whether it needs a new one.
	double ArmedGraceDeadlineSeconds = TNumericLimits<double>::Max();

	// Unregisters one grace entry's participant and drops it. The single place a grace entry ends, so an
	// avatar can never be dropped while its registry record still names it.
	void EndAvatarGrace(const FGuid& UUID, FCrowdyMassAvatarGraceEntry& Entry);

	// Releases every avatar, and with each one the entity registry record it enrolled, so every affected
	// entity has to be re-enrolled.
	// Reason names why in the log; the ids stay marked as reclaimed so a caller asking afterwards is told
	// the entity is still there and the avatar is not.
	void ReleaseAllAvatars(const TCHAR* Reason);

	void ReportAvatarEviction(const FGuid& UUID);

	// Charges one event against this entity's allowance, refilling it first. False means the allowance is
	// spent and the event must be refused.
	bool ChargeReceiveBudget(const FGuid& UUID);

	// Whether this entity is the kind the event was declared for, with the refusal reported at most once every
	// few seconds. Shared by the allowance step and the apply step so the two can never disagree about what is
	// applicable, which is the divergence this whole receive path exists to remove.
	bool PassesEventClassGuard(const FGuid& EntityUUID, const UFunction* Function);

	// Says, once per DECLARING class, that this event's body cannot run on a stand-in of StandInClass and so
	// lands nothing on entities held here as rows. Shared by the allowance step and the dispatch, and holding
	// the throttle rather than taking it from either, so which of the two refuses a given call first cannot
	// change whether an author ever hears about the declaration.
	void ReportUnhostableEventDeclaration(const UClass* StandInClass, const UFunction* Function);

	/**
	 * Works out what an object-bound handler is given for one event: the parameter its entity target is
	 * written to, and which of the call's parameters land in which of its own.
	 *
	 * The matching is BY NAME and only where both sides declare exactly the same type. A handler parameter
	 * with no counterpart on the event is named in OutUnmatched and keeps the handler's own default; it is
	 * never filled from a parameter that merely sits in the same position.
	 *
	 * False with OutRefusal set means the handler cannot receive this event at all: its first parameter is
	 * not an entity target, it declares none, or it declares an output.
	 */
	static bool BuildObjectEventHandlerPlan(const UFunction* EventFunction, const UFunction* HandlerFunction,
		const FStructProperty*& OutTargetParam, TArray<FCrowdyMassEventParamCopy>& OutParamCopies,
		TArray<FString>& OutUnmatched, FString& OutRefusal);

	// Adds one already-built registration to the list for its function and hands back its handle.
	FCrowdyMassEventHandlerHandle AddEventHandlerRegistration(const UFunction* Function,
		FCrowdyMassEventHandlerRegistration&& Registration);

	// Takes one registration out of the list the next dispatch reads. The entry itself is left alive for as
	// long as anything still holds it, which is what makes this safe to call from inside the handler being
	// removed.
	bool RemoveEventHandlerRegistration(const FCrowdyMassEventHandlerRegistration& Registration);

	/**
	 * Calls one object-bound handler with the entity target and whatever of the call's parameters it
	 * declares. False when the binding is dead and should be dropped: its object is gone, or its function
	 * is (which a Blueprint recompile does).
	 *
	 * The parameter frame lives on the stack for the length of this call, which is why it is a function of
	 * its own rather than the body of the loop that walks the handlers.
	 */
	bool DispatchObjectEventHandler(const FCrowdyMassEventHandlerRegistration& Registration,
		const FMassEntityHandle Entity, const FCrowdyEventParams& Params);

	/**
	 * Runs the event's OWN body, the one the author wrote on their actor class, on a per-class stand-in
	 * positioned at this entity. True when the body ran.
	 *
	 * That answer is measured against the engine's own conditions for running one, not against having reached
	 * the call: ProcessEvent declines silently in a world that does not run actor script, so a call that was
	 * made and a body that ran are different facts and only the second is reported here.
	 *
	 * This is what makes an event authored once behave the same on a client drawing the entity as a row as
	 * on one drawing it as an actor, for the part of a presentation body that is world-space work at the
	 * entity's own location. What it cannot carry is anything routed through the actor's own components,
	 * which a row does not have, or anything that resumes after the body returns, because the stand-in is
	 * reset and moved to the next entity immediately; the Blueprint compiler warns about both at authoring
	 * time.
	 *
	 * The frame handed in is the one the router already decoded FOR THIS FUNCTION, so there is no second
	 * signature to bridge and nothing to copy by name.
	 */
	bool DispatchEventOnProxy(const FGuid& EntityUUID, const FMassEntityHandle Entity, const UFunction* Function,
		const FCrowdyEventParams& Params);

#if WITH_EDITOR
	// Live Coding replaces class objects in place, which leaves every cached avatar pointing at a class
	// that no longer exists. Nothing here is worth keeping across that, so the whole cache goes.
	void HandleObjectsReplaced(const TMap<UObject*, UObject*>& ReplacementMap);

	// Drops the per-class animation answers when an animation set or a Mass entity config is edited in
	// place, which replaces no object and so is invisible to the hook above.
	void HandleObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);
#endif

	/** Project settings for the arrival-interval estimate, with any runtime overrides already applied. */
	FCrowdyInterpolationTuning GetInterpolationTuning() const;

	bool IsSpatialGridEnabled() const;
	ECrowdyMassSpatialGridMode GetSpatialGridMode() const;
	float GetSpatialGridCellSize() const;
	float GetSpatialGridEntityBoundsRadius() const;
	float GetSpatialGridTraceQueryRadius() const;
	int32 GetSpatialGridMaxRayCells() const;

	/** Ceiling on the swept radius one remote update may claim, since that radius is derived from wire data. */
	float GetSpatialGridMaxEntitySweepRadius() const;
	float GetSpatialGridDebugTwoDimensionalHeightPadding() const;
	/** False when Location is not finite or its cell, widened by RadiusCells, would leave the int range. */
	bool TryGetSpatialGridCell(const FVector& Location, double RadiusCells, FIntVector& OutCell) const;
	/** False when Radius is not finite or covers more than MaxCells cells along one axis. */
	bool TryGetSpatialGridRadiusCells(float Radius, int32 MaxCells, int32& OutRadiusCells) const;
	FBox GetSpatialGridDebugCellBounds(const FIntVector& Cell, const FVector& Start, const FVector& End) const;
	/** False, with no cells, when the box around the sphere would exceed MaxCells or its input is not finite. */
	bool GetSpatialGridCellsForSphere(const FVector& Origin, float Radius, int32 MaxCells, TArray<FIntVector>& OutCells) const;
	bool GetSpatialGridCellsForRay(const FVector& Start, const FVector& End, float QueryRadius, int32 MaxCells, TArray<FIntVector>& OutCells) const;
	void UpdateSpatialGridEntity(FMassEntityHandle Entity, const FVector& Origin, float Radius);
	void RemoveSpatialGridEntity(FMassEntityHandle Entity);
};
