#pragma once

#include "CoreMinimal.h"
#include "Mass/EntityElementTypes.h"
#include "Templates/Function.h"
#include "UObject/TopLevelAssetPath.h"
#include <type_traits>

struct FCrowdyMassEntityTarget;
struct FMassEntityManager;
class FProperty;
class UScriptStruct;

/**
 * A by-name door onto the fragments a game authors, so that reading and writing one of its fields is
 * something a Blueprint can do rather than something only C++ can do.
 *
 * A field is registered once, in C++, beside the view-state ingestor it belongs with and UNDER THE SAME
 * NAME. UCrowdyMassEntitySubsystem::RegisterViewStateField says what an arriving byte called "Stance"
 * does; RegisterField here says where "Stance" lives. Sharing the key is the whole point: a graph asking
 * for "Stance" and an ingestor landing it cannot drift apart, and a game that registers one and forgets
 * the other finds out by name rather than by silence.
 *
 * Two rules hold everything here up, and both are structural rather than advice.
 *
 * A fragment pointer never outlives the call that fetched it. An access hands the field's live memory to a
 * visitor that runs inside that call and then answers with a VALUE, so nothing crossing into Blueprint is a
 * pointer at all. Mass moves an entity's data whenever its archetype changes, so a pointer kept past its
 * call would be addressing the archetype the entity has left.
 *
 * Nothing here runs while Mass is processing. A Blueprint reached from a delegate broadcast inside a chunk
 * loop would otherwise write a fragment during the schedule, racing every processor declared to read it,
 * and that is a real path rather than a hypothetical one. An ordinary tick that overlaps a Mass phase is
 * the same race, arrived at differently: phases run alongside the game thread rather than blocking it.
 * Either call is refused and reported instead of being performed; writing from a phase-end delegate, or
 * from a broadcast deferred to the end of the frame, puts the same code outside the schedule.
 *
 * Game thread only, all of it.
 */
namespace CrowdyMassFields
{
	/**
	 * Whether an access happened, and when it did not, which reason it was.
	 *
	 * They are separate answers because they call for different fixes. A stale handle or a missing fragment
	 * is ordinary and temporary. An unregistered name or a type mismatch is an authoring mistake and will
	 * never start working on its own. A refusal during Mass processing says nothing is wrong with the graph
	 * at all, only with when something is calling it.
	 */
	enum class EAccessResult : uint8
	{
		// The field was read or written.
		Ok,

		// The target names no world, or that world holds no Mass entity manager. Only the target-resolving
		// entry points can answer this; the ones given a manager never do.
		NoWorld,

		// The entity manager does not hold a live, built entity under this handle: despawned, or stale.
		InvalidEntity,

		// No field is registered under this name.
		UnknownField,

		// The entity carries no fragment of the registered type. Ordinary: an entity built from a different
		// config legitimately lacks fragments another one has.
		NoFragment,

		// The field exists but is not of the type being read or written as.
		TypeMismatch,

		// Mass is running processors right now, so this would have raced them. Refused.
		DuringMassProcessing,

		// Called off the game thread.
		OffGameThread
	};

	/**
	 * Where one registered name lives: which fragment type, and which property inside it.
	 *
	 * The two pointers are the resolved answer and are only valid while the engine's reflection data is.
	 * Recompiling native code replaces a fragment's UScriptStruct and destroys the FProperty objects the old
	 * one owned, so both go stale together and neither can be re-resolved from itself: reading either one
	 * after that is the dangling access, not the fix for it.
	 *
	 * The two names below are what a binding is re-resolved FROM. They are set once at registration, are
	 * never rewritten, and hold no reference to anything that a recompile can destroy.
	 */
	struct FFieldBinding
	{
		const UScriptStruct* FragmentType = nullptr;
		const FProperty* Property = nullptr;

		// The fragment type by path, as /Script/<Module>.<Struct>.
		FTopLevelAssetPath FragmentTypePath;

		// The property inside it, by name.
		FName PropertyName;
	};

	/**
	 * Binds FieldName to one property of one fragment type, so a Blueprint can read and write it by name.
	 * Returns false, having said why, when the binding cannot be made.
	 *
	 * FragmentPropertyName must name a UPROPERTY. A plain C++ member of a USTRUCT has no reflection entry
	 * and cannot be found by name at all, so registering one is refused here rather than presenting later
	 * as a field that is registered and never resolves.
	 *
	 * Registering the same name twice with the SAME binding is a no-op and succeeds, which is what makes it
	 * safe to register from something that runs once per world. Registering it with a DIFFERENT binding is
	 * refused and the first binding is kept: a name is claimed process-wide, so the alternative is one
	 * plugin's registration silently answering for another's field.
	 *
	 * Registering a field grants Blueprint write access to it. Register only fields that are yours and that
	 * are safe to draw from: anything mechanical, and anything a peer must not be able to influence,
	 * has to be decided by the server rather than sit behind this door.
	 */
	CROWDYMASS_API bool RegisterField(FName FieldName, const UScriptStruct* FragmentType, FName FragmentPropertyName);

	/** RegisterField with the fragment type named by C++ type rather than by its script struct. */
	template<typename FragmentType>
	bool RegisterField(const FName FieldName, const FName FragmentPropertyName)
	{
		static_assert(std::is_base_of_v<FMassFragment, FragmentType>,
			"A registered field lives on a Mass fragment; the type must derive from FMassFragment.");

		return RegisterField(FieldName, FragmentType::StaticStruct(), FragmentPropertyName);
	}

	/** The binding registered under this name, or null when there is none. */
	CROWDYMASS_API const FFieldBinding* FindField(FName FieldName);

	/**
	 * True when a field is registered under this name. A game's startup check can use this to notice that
	 * its own registrations never ran, which is otherwise silent: every access simply answers UnknownField.
	 */
	CROWDYMASS_API bool IsFieldRegistered(FName FieldName);

	/**
	 * Re-resolves every registered binding against the reflection data that exists right now, and returns how
	 * many stopped resolving and were dropped.
	 *
	 * Editor builds call this for you: it is bound to the engine's reload-complete notification, which Live
	 * Coding and hot reload both broadcast once they have finished rebuilding native reflection. A recompile
	 * replaces the UScriptStruct a binding was registered against and destroys the properties the old one
	 * owned, so without this the next by-name access would reach into a destroyed property, on the one path a
	 * developer uses constantly while iterating.
	 *
	 * Bindings are re-resolved rather than dropped. Registrations run from startup or from a world's setup,
	 * so a registry emptied mid-session would never be filled again and every access for the rest of that
	 * session would answer UnknownField: a surface that is silently and permanently dead, which is worse than
	 * the crash it would be replacing. Only a binding whose fragment type or property is genuinely gone is
	 * dropped, and each one is reported as it goes.
	 *
	 * Safe to call directly, and idempotent: re-resolving a binding that has not moved writes back what it
	 * already held.
	 */
	CROWDYMASS_API int32 RebindRegisteredFields();

	/**
	 * Given the field's property and a pointer to its live value inside the entity's fragment, reads or
	 * writes it and returns whether the property was of the expected type.
	 *
	 * ValuePtr is valid for the duration of the visitor call and no longer. Never store it, never hand it
	 * across a call boundary, and never keep anything derived from its address.
	 */
	using FFieldVisitor = TFunctionRef<bool(const FProperty& Property, void* ValuePtr)>;

	/**
	 * Resolves FieldName on the entity Target names inside EntityManager and runs Visitor against it.
	 *
	 * bWrite changes nothing about what is resolved. It says which of the two a refused call was trying to
	 * do, so the report names the actual hazard, and a write refused during Mass processing is reported as
	 * the defect it is rather than as a note.
	 *
	 * Target.World is NOT read here, which is why this never answers NoWorld. It is the entry point for a
	 * caller that already holds the manager (and for a test, which otherwise could not reach any of these
	 * outcomes without standing up a PIE world). Nothing checks that the manager and the target belong
	 * together, and nothing can: an index and serial from another world can name a live entity in this one.
	 */
	CROWDYMASS_API EAccessResult AccessFieldIn(FMassEntityManager& EntityManager, const FCrowdyMassEntityTarget& Target,
		FName FieldName, bool bWrite, FFieldVisitor Visitor);

	/**
	 * AccessFieldIn against the entity manager of the world the target itself names. This is the form
	 * ordinary gameplay code calls: the target carries the world, so nothing else has to.
	 */
	CROWDYMASS_API EAccessResult AccessField(const FCrowdyMassEntityTarget& Target, FName FieldName, bool bWrite,
		FFieldVisitor Visitor);

	/**
	 * The Mass entity manager belonging to the world a target names, or null when that world has gone or
	 * holds none.
	 *
	 * Every target-keyed entry point goes through this one lookup, which is what lets a target be the whole
	 * address a caller has to supply: the handle says which entity, and the world beside it says which
	 * manager that handle is an index into. Handles are not unique across managers.
	 */
	CROWDYMASS_API FMassEntityManager* ResolveEntityManager(const FCrowdyMassEntityTarget& Target);

	/**
	 * Writes the pose byte on the entity Target names, under the same guards a registered field write runs
	 * under.
	 *
	 * The pose has its own entry point rather than going through the registry because the fragment holding
	 * it carries no UPROPERTY, so nothing can find it by name. PoseId is an authored row position, which is
	 * why this takes an id rather than a name: turning a name into an id needs the animation set, which is
	 * reached through the world subsystem. UCrowdyMassTargetLibrary::SetEntityPose is the by-name form.
	 */
	CROWDYMASS_API EAccessResult SetPoseIdIn(FMassEntityManager& EntityManager, const FCrowdyMassEntityTarget& Target,
		uint8 PoseId);

	/** A sentence naming what a result means, for a log line or a test failure message. */
	CROWDYMASS_API const TCHAR* DescribeAccessResult(EAccessResult Result);

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Test seam: the entity manager ResolveEntityManager answers with, in place of the one a target's own
	 * world would supply.
	 *
	 * A headless test has no PIE world to hold a Mass subsystem, so without this every target-keyed entry
	 * point would answer NoWorld and nothing past that lookup could be reached at all. Null restores the
	 * ordinary resolution.
	 */
	CROWDYMASS_API void SetEntityManagerForTest(FMassEntityManager* InEntityManager);

	/**
	 * Drops one registration, so a test can register a fixture field and leave the registry as it found it.
	 * Deliberately one name at a time: the registry is process-wide, and a test running in the editor
	 * shares it with whatever the loaded game registered at startup.
	 */
	CROWDYMASS_API void UnregisterFieldForTest(FName FieldName);

	/**
	 * Replaces a registered binding with the given one verbatim, and returns false when nothing is registered
	 * under that name.
	 *
	 * The seam a rebind test needs. A real code reload cannot be performed from inside a running test, so a
	 * test stands the registry in the state a reload leaves it in: pointers that address the wrong thing, or
	 * an identity naming something that no longer exists. Nothing here is validated, which is the point.
	 */
	CROWDYMASS_API bool OverwriteFieldBindingForTest(FName FieldName, const FFieldBinding& Binding);

	/**
	 * True when the reload-complete hook that re-resolves bindings is bound. Always false outside an editor
	 * build, where nothing can reload native code and nothing is bound.
	 */
	CROWDYMASS_API bool IsReloadRebindInstalledForTest();

	/**
	 * Forgets which refusals have already been reported, so the next one is reported again.
	 *
	 * The throttle behind the refusal messages is process-wide and time-based, which would otherwise make
	 * any test that asserts a refusal was reported depend on what ran before it and how recently.
	 */
	CROWDYMASS_API void ResetRefusalLogThrottleForTest();
#endif
}
