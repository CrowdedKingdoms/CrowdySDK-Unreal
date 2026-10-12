#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "UObject/ObjectPtr.h"

class AActor;
class FProperty;
class UClass;
class UWorld;

/**
 * A stand-in of an actor class that was never spawned, so an inbound event's own body can run for an
 * entity this client holds as a row rather than as an actor.
 *
 * ONE PER CLASS is enough, and that is the whole reason this shape affords a crowd: dispatch is sequential
 * on the game thread, so at most one proxy is ever in use, and it is borrowed for a single event and handed
 * straight back. Memory is O(distinct classes), never O(entities).
 *
 * A proxy is not in the world. It is outered to the world's persistent level, which is the only thing
 * AActor::GetWorld needs, and is never added to that level's actor list, never registered, never ticked and
 * never rendered. Its components exist and none of them is registered, so a call on one succeeds and
 * reaches nothing: what a body may usefully do here is world-space work at the entity's own position.
 */
namespace CrowdyMassDispatchProxy
{
	/**
	 * Builds a proxy of ActorClass in World, or null when either is unusable (no persistent level, not an
	 * actor class, abstract). The caller owns keeping it alive; nothing else references one.
	 */
	AActor* Construct(UWorld* World, UClass* ActorClass);

	/**
	 * Returns a borrowed proxy to its class defaults so nothing an event body wrote carries into the next
	 * entity's dispatch.
	 *
	 * Properties pointing at the proxy's OWN subobjects are skipped. A wholesale copy from the class
	 * default object would overwrite those with the default object's components, leaving every body from
	 * then on operating on a mesh shared by every entity and belonging to an object that must never be
	 * written to at all. Recognised by where the value lives as well as by the instanced-reference flag,
	 * because ownership is the property that matters and the flag is only evidence of it.
	 *
	 * Direct object references only. A class holding its components in an array or a map would have those
	 * clobbered and this would not prevent it.
	 */
	void ResetToClassDefaults(AActor* Proxy);

	/**
	 * Splits a freshly built proxy's properties into the ones a reset copies from the class default object
	 * and the ones pointing at the proxy's own subobjects, with the values those held, worked out once.
	 *
	 * Deciding this per event costs a walk of the whole property chain plus an outer walk for every object
	 * property, on the receive path, for every inbound entity event.
	 */
	void BuildResetPlan(const AActor* Proxy, TArray<FProperty*>& OutResetProperties,
		TArray<FProperty*>& OutSubobjectProperties, TArray<TObjectPtr<UObject>>& OutSubobjectValues);

	/**
	 * The same reset against a plan BuildResetPlan already worked out for this proxy.
	 *
	 * The subobject properties are RESTORED rather than skipped. Skipping them is only correct while they
	 * still hold what they held at construction, and a body that reassigns one would otherwise keep that
	 * reassignment for every entity dispatched afterwards.
	 */
	void ResetToClassDefaults(AActor* Proxy, TConstArrayView<FProperty*> ResetProperties,
		TConstArrayView<FProperty*> SubobjectProperties, TConstArrayView<TObjectPtr<UObject>> SubobjectValues);

	/**
	 * Whether an inbound event's own body is run on a stand-in at all.
	 *
	 * Off, an event nothing registered a handler for lands nothing beyond the action write a declared
	 * action already gets, which is what a client did before the stand-in existed. Driven by
	 * crowdy.mass.event.proxy outside Shipping, where it is always on.
	 */
	bool IsEnabled();

	// !UE_BUILD_SHIPPING as well as the test guard: the scope writes the console variable behind IsEnabled, and
	// that variable only exists outside Shipping. WITH_DEV_AUTOMATION_TESTS can be forced on in a Shipping
	// build, where this would then fail to compile. A test scope there would say nothing anyway: IsEnabled is a
	// hard true under Shipping, so the dispatch cannot be turned off.
#if WITH_DEV_AUTOMATION_TESTS && !UE_BUILD_SHIPPING
	/**
	 * Turns the dispatch off for the life of the scope, so a case measuring "with it off" reads the shipped
	 * switch rather than a second code path written to agree with it.
	 */
	struct FScopedDisableForTest
	{
		FScopedDisableForTest();
		~FScopedDisableForTest();

		FScopedDisableForTest(const FScopedDisableForTest&) = delete;
		FScopedDisableForTest& operator=(const FScopedDisableForTest&) = delete;

	private:
		int32 Previous = 1;
	};
#endif
}
