#pragma once

#include "Animation/AnimSequenceTransformProviderData.h"
#include "Assets/CrowdyMassAnimSet.h"
#include "CoreMinimal.h"
#include "Mass/EntityElementTypes.h"
#include "CrowdyMassAnimFragments.generated.h"

// Shared by every index-shaped field below (SequenceIndex, action slots): no index has been
// resolved or activated yet.
inline constexpr int32 CrowdyMassAnimIndexNone = INDEX_NONE;

/**
 * The per-entity animation state the animation processor owns, and the state it reads to decide
 * what to push to the shared instanced-skinned-mesh animation bank.
 *
 * The processor composes one animation out of these fields: the entity's pose, its one-shot action,
 * and the gait derived from movement, ordered by the pose's authored rank. Each step is a single
 * early return, so adding one does not reorder the others.
 *
 * Every field here is read for every entity on every frame, so the declaration order is chosen to
 * pack rather than to read well: widest first, with the single-byte members grouped at the end.
 * Interleaving them instead costs three or four bytes of padding each, which is the difference
 * between two entities per cache line and one. The assertion below holds that shape; a field added
 * in the wrong place breaks the build rather than quietly costing a cache line per entity.
 */
USTRUCT()
struct CROWDYMASS_API FCrowdyMassAnimStateFragment : public FMassFragment
{
	GENERATED_BODY()

	// The bank sequence index currently pushed for this entity. CrowdyMassAnimIndexNone means
	// nothing has been pushed yet.
	int32 SequenceIndex = CrowdyMassAnimIndexNone;

	float PlayRate = 1.0f;

	// Seconds to blend over when the pushed sequence changes. Zero snaps. Whoever writes the
	// sequence index must write this too: the renderer keeps the last value it was given, so leaving
	// it alone means inheriting whatever the previous writer set and two entities showing the same
	// sequence can blend differently.
	float BlendSeconds = 0.0f;

	// The transform interpolator zeroes velocity the instant it finishes interpolating between
	// server updates, so raw velocity magnitude is a sawtooth at the update rate. Gait selection
	// reads this smoothed value instead, never the velocity fragment directly.
	float SmoothedSpeed = 0.0f;

	/**
	 * The gait selected on the previous frame, as a one-based index into the owning anim set's Gaits
	 * array: id 1 is Gaits[0], matching the convention PoseId and ActionId use for their own arrays.
	 *
	 * Gait selection needs the previous answer: whether a given speed means walking or jogging depends
	 * on which of the two the entity is already in, which is what stops an entity sitting on a
	 * threshold from switching animation every frame.
	 *
	 * 0 means no gait has been resolved for this entity, which is what an entity holds before its first
	 * animation pass and what a set authoring no gait rows always answers. It is a starting state, not
	 * something a sender clears: a gait is derived locally from how the entity moves, so nothing off the
	 * network can ever put this back to 0. Selection leaves 0 on the first pass over any set with an
	 * authored row, by starting such an entity on the floor row.
	 */
	uint8 Gait = 0;

	EAnimSequenceTrackLoopMode LoopMode = EAnimSequenceTrackLoopMode::Loop;

};

// Four four-byte members and two single-byte members, which pack into the fifth four-byte word
// with no padding between them. Declare wider members before narrower ones; this fragment is read
// for every entity every frame, and a field added in the wrong place costs a cache line per entity
// rather than failing to build.
static_assert(sizeof(FCrowdyMassAnimStateFragment) == 4 * sizeof(int32) + 4,
	"FCrowdyMassAnimStateFragment has gained padding. Declare wider members before narrower ones; this fragment is read for every entity every frame.");

/**
 * The pose this entity is being asked to hold: a stance, a mount, a corpse, whatever a game has
 * authored. 0 means none. Otherwise a 1-based index into the owning anim set's Poses array (id 1 is
 * Poses[0], and so on), the same convention FCrowdyMassActionFragment::ActionId uses for Actions, so
 * the animation processor resolves it with a bounds check and an array index rather than a name
 * search on the hot path.
 *
 * This one byte is the whole extension point for game-specific animation state. Whether the pose
 * beats a one-shot action is authored on the pose itself, so setting this is the only thing a game
 * ever has to do, and a game with two states that could both apply chooses which id it writes rather
 * than leaving this plugin to guess between them.
 *
 * A game does not normally write this itself. It replicates a property of its own state enum, and the
 * arriving value becomes an id through UCrowdyMassAnimSet::FindPoseIdByStateValue, once per arriving
 * delta and never per frame; a value no row authors becomes 0 and clears the pose, which is why a
 * living player needs no authored row. UCrowdyMassAnimSet::FindPoseId does the same by name for local
 * code, and UCrowdyMassEntitySubsystem::SetEntityPose does the whole job by name for a caller that has
 * a UUID and no interest in ids at all.
 *
 * It has a fragment to itself, and no processor writes it, which is the point. Its writers are the
 * view-state ingestor and game code, both of which run outside the Mass schedule: a field they share
 * with a processor would be an undeclared write racing a declared one. Here every processor is a
 * reader, so the worst an ill-timed write can do is show the previous pose for one more frame.
 */
USTRUCT()
struct CROWDYMASS_API FCrowdyMassViewPoseFragment : public FMassFragment
{
	GENERATED_BODY()

	uint8 PoseId = 0;
};

/**
 * A one-shot action slot (an attack swing, an emote, and similar).
 *
 * Action-active is ActionId != 0, a FRAGMENT FIELD, deliberately never a Mass tag: adding or
 * removing a tag moves the entity to a different archetype, and archetype moves are already the
 * dominant per-entity cost in this system. Flipping a field costs nothing.
 */
USTRUCT()
struct CROWDYMASS_API FCrowdyMassActionFragment : public FMassFragment
{
	GENERATED_BODY()

	// 0 means no action is active. Otherwise a 1-based index into the owning anim set's Actions
	// array (ActionId 1 is Actions[0], and so on), so the animation processor can resolve it
	// without a name lookup on the hot path. Whoever triggers an action owns keeping this in sync
	// with the anim set the entity actually renders through.
	int32 ActionId = 0;

	int32 Variant = 0;

	float PlayRate = 1.0f;

	// Incremented on every (re)trigger, including a trigger that repeats the same ActionId. The
	// animation processor compares it against AppliedSerial below to know whether a trigger is
	// still waiting to be handed to the renderer.
	int32 Serial = 0;

	// Seconds accumulated since this action was triggered. The engine's playback clock starts
	// counting from the moment the animation processor applies it, not from the moment this
	// fragment was written, so an untouched Position of 0 would make a delayed action visibly
	// start late; the animation processor reads this once, on the first frame it applies the
	// action, to correct Position. Kept ticking for the rest of the action's life after that too:
	// the action processor compares it against the action's authored duration to know when the
	// action has finished and ActionId should be cleared. Whoever triggers an action resets this
	// to zero as it increments Serial below.
	float ElapsedSecondsSinceTrigger = 0.0f;

	// How long this action runs for, copied here by the animation processor at the moment it pushes
	// the action, because that is where the value already is: composing the action resolved it out
	// of the shared table for free.
	//
	// Carried on the entity rather than looked up again by the pass that expires it, which is the
	// whole point. Resolving a duration means finding the bank on the mesh description and searching
	// the anim set by name, so doing it per entity per frame would put back exactly the cost the
	// shared resolved table exists to remove. Here the expiry pass is an add and a compare against
	// two floats already in the cache line it is reading.
	float DurationSeconds = 0.0f;

	// The Serial whose animation has actually been handed to the renderer, which is not the same
	// question as which Serial was most recently written. The animation processor sets it at the
	// exact frame it pushes, so a trigger that has not been shown yet is Serial != AppliedSerial.
	//
	// It records the Serial rather than a "corrected yet" flag because a flag cannot say WHICH
	// trigger it was set for. Two triggers landing between two animation passes would leave the
	// flag reading "already handled" for a trigger the renderer never saw, and a slot chosen from
	// the Serial's parity would land back on the index already showing, so the push would be
	// skipped as a no-op. That is the exact silence the authored slot pair exists to prevent.
	int32 AppliedSerial = 0;
};

/**
 * Shared by every entity built from the same anim trait, so the resolved anim set lives once per
 * archetype instead of being copied onto every entity.
 */
USTRUCT()
struct CROWDYMASS_API FCrowdyMassAnimSetSharedFragment : public FMassConstSharedFragment
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<const UCrowdyMassAnimSet> AnimSet = nullptr;
};
