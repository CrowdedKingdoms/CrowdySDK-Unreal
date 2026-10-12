#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"

class UActorComponent;
class UClass;
class UMassEntityConfigAsset;
class UObject;
class UMassEntityTraitBase;

/**
 * Which of the three ways a component reaches an actor supplied a trait declaration.
 *
 * The three do not see each other, so which one answered is the first thing an author needs when the answer
 * is not the one they expected: traits listed on a parent's construction script and traits listed on the
 * class in front of them look identical from the outside.
 */
enum class ECrowdyMassEntityTraitsRoute : uint8
{
	/** Nothing declared any traits, so no route answered. */
	None,

	/** A component declared in C++, which lives on the class default object. */
	NativeComponent,

	/** A component added in the Blueprint editor, which exists only as a construction script template. */
	BlueprintComponent,

	/** An inherited component whose defaults a child Blueprint changed, which is kept in neither of those. */
	InheritedOverride
};

/** A route in words, for a log line or a message shown to whoever authored the class. */
const TCHAR* LexToString(ECrowdyMassEntityTraitsRoute Route);

/** Which route Component, handed back by AActor::ForEachComponentOfActorClassDefault, reached a class by, given that class's default object. */
ECrowdyMassEntityTraitsRoute RouteOfClassDefaultComponent(const UActorComponent& Component, const UObject* DefaultObject);

/**
 * What traits an actor class declares for itself, worked out from the class alone.
 *
 * Traits are RAW pointers to the instances the class's component owns, and this struct is meant to be read
 * and thrown away by the caller that asked for it. They must never be stored: they belong to a component
 * template on a class, and a caller that wants to keep one has to copy it into its own ownership first,
 * because recompiling that class replaces every object the template owns.
 *
 * bSawComponent separates a class carrying the component with nothing in it from a class carrying no
 * component at all: two different messages, one indistinguishable result. EmptyEntryCount is the third of
 * those states and needs its own remedy again, because a row left set to none is one row to fill in rather
 * than a whole list to populate or a component to add.
 *
 * Route, ComponentName and RouteOwnerName describe where the declaration was found rather than what was
 * found, and are only filled in when one was.
 */
struct FCrowdyMassDerivedEntityTraits
{
	/** The traits the answering component lists, in the order it lists them, with any unset rows left out. */
	TArray<const UMassEntityTraitBase*> Traits;

	/** True when the class carries a Crowdy Mass Entity Traits component, whether or not it lists anything. */
	bool bSawComponent = false;

	/** How many rows of the answering component's list were left set to none. */
	int32 EmptyEntryCount = 0;

	/** How many FURTHER such components the class carries beyond the one that answered, which are not read. */
	int32 ShadowedComponentCount = 0;

	ECrowdyMassEntityTraitsRoute Route = ECrowdyMassEntityTraitsRoute::None;

	/** The component the declaration was read off, for a class carrying more than one. */
	FString ComponentName;

	/** What holds that component: the class default object for a native one, otherwise the Blueprint class
	 * whose construction script or override handler carries the template. */
	FString RouteOwnerName;

	/** Empty when at least one trait was declared. Otherwise what is missing and what to change, in that order. */
	FString Reason;

	bool IsValid() const
	{
		return Traits.Num() > 0;
	}
};

/**
 * Reads the Mass traits an actor class declares on itself, without spawning anything and without a world.
 *
 * FromClass walks a class default object and every Blueprint in its parent chain, which is far too much to
 * repeat per entity. Ask once per class, keep the answer, and never call it while updating entities.
 */
struct FCrowdyMassEntityTraitsDerivation
{
	/** The traits ActorClass lists on its own component, or an empty list and a reason explaining why. */
	static FCrowdyMassDerivedEntityTraits FromClass(const UClass* ActorClass);

	/**
	 * Whether Declared can be added to a config that already carries the traits worked out for a class.
	 *
	 * OutRefusal is filled in and false returned when adding it would produce a config that reads as
	 * configured and draws wrong.
	 *
	 * OutOverridden names the trait class this one replaces, when it replaces one, which is NOT a refusal: a
	 * class replacing what was worked out for it is the point of declaring traits at all, and saying so is
	 * what lets an author find out it happened. Mass decides the replacement itself, by keeping the last
	 * trait of a class it is offered, so this only reports what that will do rather than doing it.
	 *
	 * Config is read as its own trait list rather than its combined one. A config built from a class has no
	 * parent, so the two are the same thing, and asking for the combined list would need a build context.
	 */
	static bool CanAppend(const UMassEntityConfigAsset& Config, const UMassEntityTraitBase& Declared,
		FString& OutRefusal, FString& OutOverridden);
};
