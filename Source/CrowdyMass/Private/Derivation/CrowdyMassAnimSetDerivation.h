#pragma once

#include "Containers/UnrealString.h"
#include "UObject/SoftObjectPtr.h"

class UClass;
class UCrowdyMassAnimSet;
class UMassEntityConfigAsset;

/**
 * Which of the three ways a component reaches an actor supplied an animation set declaration.
 *
 * The three do not see each other, so which one answered is the first thing an author needs when the
 * answer is not the one they expected: a set named on a parent's construction script and one named on the
 * class in front of them look identical from the outside.
 */
enum class ECrowdyMassAnimSetRoute : uint8
{
	/** Nothing declared a set, so no route answered. */
	None,

	/** A component declared in C++, which lives on the class default object. */
	NativeComponent,

	/** A component added in the Blueprint editor, which exists only as a construction script template. */
	BlueprintComponent,

	/** An inherited component whose defaults a child Blueprint changed, which is kept in neither of those. */
	InheritedOverride
};

/** A route in words, for a log line or a message shown to whoever authored the class. */
const TCHAR* LexToString(ECrowdyMassAnimSetRoute Route);

/**
 * What animation set an actor class declares for itself, worked out from the class alone.
 *
 * AnimSet is the reference the component named and is deliberately NOT loaded here. Loading is a
 * synchronous asset load, which belongs where the answer is kept rather than where it is worked out, and
 * it is also the only place that can tell "named nothing" apart from "named something that would not
 * load": those are different faults with different fixes and both look like a null set.
 *
 * bSawComponent separates a class carrying the component with an empty slot from a class carrying no
 * component at all, for the same reason: two different messages, one indistinguishable result.
 *
 * Route, ComponentName and RouteOwnerName describe where the declaration was found rather than what was
 * found, and are only filled in when one was.
 */
struct FCrowdyMassDerivedAnimSet
{
	TSoftObjectPtr<UCrowdyMassAnimSet> AnimSet;

	/** True when the class carries a Crowdy Mass Anim Set component, whether or not it names anything. */
	bool bSawComponent = false;

	ECrowdyMassAnimSetRoute Route = ECrowdyMassAnimSetRoute::None;

	/** The component the declaration was read off, for a class carrying more than one. */
	FString ComponentName;

	/** What holds that component: the class default object for a native one, otherwise the Blueprint
	 * class whose construction script or override handler carries the template. */
	FString RouteOwnerName;

	/** Empty when a set was declared. Otherwise what is missing and what to change, in that order. */
	FString Reason;

	bool IsValid() const
	{
		return !AnimSet.IsNull();
	}
};

/**
 * What a Mass entity config says about animation, which is a different question from whether the config
 * exists.
 *
 * A config that carries no animation trait does not answer "this class has no animation". It simply does
 * not answer, and the class's own declaration is asked next. Keeping those apart is the whole reason this
 * reports bSawAnimationTrait rather than only a set: a config authored for collision or level-of-detail
 * reasons would otherwise silently disable animation for a class that names a set elsewhere.
 */
struct FCrowdyMassConfigAnimSet
{
	TSoftObjectPtr<UCrowdyMassAnimSet> AnimSet;

	/** True when the config carries a Crowdy Mass Animation trait at all. */
	bool bSawAnimationTrait = false;

	bool IsValid() const
	{
		return !AnimSet.IsNull();
	}
};

/**
 * Reads the animation set an actor class or a Mass entity config declares, without spawning anything and
 * without a world. Both answers depend only on what was read, so a caller is free to work each out once
 * and keep it.
 *
 * FromClass walks a class default object and every Blueprint in its parent chain, which is far too much to
 * repeat per entity. Ask once per class, keep the answer, and never call it while updating entities.
 */
struct FCrowdyMassAnimSetDerivation
{
	/** The set ActorClass declares on its own component, or a null reference and a reason explaining why. */
	static FCrowdyMassDerivedAnimSet FromClass(const UClass* ActorClass);

	/**
	 * The set Config's animation trait names, and whether it carries such a trait at all.
	 *
	 * The FIRST animation trait, because that is the one whose set the entities built from this config
	 * actually render through: a config carrying two is a configuration mistake with no right answer
	 * available here, and picking the first at least matches what the trait itself does when it installs
	 * the shared fragment.
	 */
	static FCrowdyMassConfigAnimSet FromConfig(const UMassEntityConfigAsset* Config);
};
