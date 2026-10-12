#pragma once

#include "Containers/UnrealString.h"
#include "UObject/SoftObjectPtr.h"

class AActor;
class UClass;

/**
 * Which of the three ways a component reaches an actor supplied a near tier shell declaration.
 *
 * The three do not see each other, so which one answered is the first thing an author needs when the
 * answer is not the one they expected: a shell named on a parent's construction script and one named on
 * the class in front of them look identical from the outside.
 */
enum class ECrowdyMassNearTierRoute : uint8
{
	/** Nothing declared a shell, so no route answered. */
	None,

	/** A component declared in C++, which lives on the class default object. */
	NativeComponent,

	/** A component added in the Blueprint editor, which exists only as a construction script template. */
	BlueprintComponent,

	/** An inherited component whose defaults a child Blueprint changed, which is kept in neither of those. */
	InheritedOverride
};

/** A route in words, for a log line or a message shown to whoever authored the class. */
const TCHAR* LexToString(ECrowdyMassNearTierRoute Route);

/**
 * What near tier shell an actor class declares for itself, worked out from the class alone.
 *
 * NearTierActorClass is the reference the component named and is deliberately NOT loaded here. Loading
 * is a synchronous class load, which belongs where the answer is used rather than where it is worked
 * out, and it is also the only place that can tell "named nothing" apart from "named something that
 * would not load": those are different faults with different fixes and both look like a null class.
 *
 * bSawComponent separates a class carrying the component with an empty slot from a class carrying no
 * component at all, for the same reason: two different messages, one indistinguishable result.
 *
 * Route, ComponentName and RouteOwnerName describe where the declaration was found rather than what was
 * found, and are only filled in when one was.
 */
struct FCrowdyMassDerivedNearTierShell
{
	TSoftClassPtr<AActor> NearTierActorClass;

	/** True when the class carries a Crowdy Mass Near Tier Shell component, whether or not it names anything. */
	bool bSawComponent = false;

	ECrowdyMassNearTierRoute Route = ECrowdyMassNearTierRoute::None;

	/** The component the declaration was read off, for a class carrying more than one. */
	FString ComponentName;

	/** What holds that component: the class default object for a native one, otherwise the Blueprint
	 * class whose construction script or override handler carries the template. */
	FString RouteOwnerName;

	/** Empty when a shell was declared. Otherwise what is missing and what to change, in that order. */
	FString Reason;

	bool IsValid() const
	{
		return !NearTierActorClass.IsNull();
	}
};

/**
 * Reads the near tier shell an actor class declares, without spawning anything and without a world. The
 * answer depends only on what was read, so a caller is free to work it out once and keep it.
 *
 * FromClass walks a class default object and every Blueprint in its parent chain, which is far too much
 * to repeat per entity. Ask once per class, keep the answer, and never call it while updating entities.
 */
struct FCrowdyMassNearTierDerivation
{
	/** The shell ActorClass declares on its own component, or a null reference and a reason explaining why. */
	static FCrowdyMassDerivedNearTierShell FromClass(const UClass* ActorClass);

	/**
	 * Whether ShellActorClass is something an entity can actually be promoted TO.
	 *
	 * Being an Actor class is not the test. Everything a promoted entity does is done by its Crowdy Mass
	 * Near Tier component: it binds the actor to the entity, follows the entity's transform, and carries
	 * the entity's trace collision onto it. A named class without one is spawned, enabled and placed once,
	 * and then stands still where it was promoted while its entity walks away, with its crowd row no longer
	 * drawn and the entity no longer hittable. From the outside that is indistinguishable from a working
	 * promotion, so it is refused before a template is built rather than diagnosed afterwards.
	 *
	 * Reads class defaults, like FromClass, so it is worked out once per class and never per entity.
	 */
	static bool CanHostNearTierView(const UClass* ShellActorClass);

	/**
	 * The declared shell loaded and checked, or null when there is nothing an entity could be promoted to.
	 *
	 * Two different nulls, told apart by OutRefusal. An EMPTY refusal means the class declared no shell,
	 * which is every class in a project that has never asked for a near tier and is not a fault. A filled
	 * one means a shell was declared and cannot be used, and says which of the two ways it cannot: named a
	 * class that would not load, or named one carrying no view component to drive it.
	 *
	 * Both the authored and the derived config paths ask this, so the checks and the wording stay one thing.
	 * A second copy of them would drift, and the failure that drift produces is a class promoted on one path
	 * and silently left as crowd rows on the other.
	 *
	 * bOutAskAgainLater separates a refusal that is an authoring mistake from one that is a moment in time.
	 * A class that has not finished forming has no class default object to read components off and answers
	 * the same null as a class carrying none, so a caller that memoises the answer would make a transient
	 * state permanent. Nothing else can tell those apart once the answer has been handed back.
	 *
	 * Loads a class synchronously, so it belongs where a config is worked out, once per class, never per
	 * entity.
	 */
	static UClass* LoadUsableShellClass(const UClass* EntityClass, const FCrowdyMassDerivedNearTierShell& Declared,
		FString& OutRefusal, bool& bOutAskAgainLater);
};
