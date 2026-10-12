#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "CrowdyMassEntityTraitsComponent.generated.h"

class UMassEntityTraitBase;

/**
 * Adds Mass traits of your own to the entity config worked out from this actor's class.
 *
 * Add this to a character that has no Mass entity config authored for it. Such a class is drawn from a
 * config built from the class itself, which carries what a crowd needs and nothing a particular game needs.
 * The traits listed here are added to that config, so one fragment of your own costs one component rather
 * than authoring the whole config by hand and giving up everything the class supplies for free.
 *
 * They are added LAST. Mass combines a config's traits by class and keeps the last one offered, so a trait
 * listed here of the same class as one that would have been worked out replaces it, which is what makes this
 * a way to change the derived config rather than only to extend it. A trait of a DIFFERENT class that does
 * the same job replaces nothing and both build, which is why a second visualization trait is refused with a
 * message rather than added.
 *
 * A class that DOES have a Mass entity config authored for it is built from that config alone and nothing
 * here is read: a config's trait list is the whole statement of what its entities are made of, and adding to
 * it from a component would give a project traits its config never lists. Such a class is told so once, so
 * that filling this in and seeing nothing happen is never silent.
 *
 * The component holds no per-instance state and does nothing at runtime. It is read off the CLASS, before
 * anything is spawned, so the answer is the same for every actor of that class and is worked out once.
 */
UCLASS(ClassGroup=(CrowdyMass), meta=(BlueprintSpawnableComponent, DisplayName="Crowdy Mass Entity Traits"))
class CROWDYMASS_API UCrowdyMassEntityTraitsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCrowdyMassEntityTraitsComponent();

	/**
	 * The traits added to the config built from this class, in the order listed.
	 *
	 * Leaving the list empty, and leaving a row in it set to none, are reported as two different mistakes,
	 * because they have two different remedies: one is a list to fill in and the other is a single row to
	 * pick a trait class on. Both are different again from a class carrying no component at all, which is
	 * the ordinary case and says nothing.
	 *
	 * A class carrying more than one of these components answers from the first one that lists anything, and
	 * says so; put every trait on one component. A component whose list is empty is walked past rather than
	 * taken, so a base class can expose the slot and leave a derived class to fill it in. Which of two
	 * FILLED-IN lists answers is not worth relying on: a component declared in C++ is read before one added
	 * in a Blueprint, whatever the inheritance order between them.
	 */
	UPROPERTY(EditAnywhere, Category="Crowdy Mass|Traits", Instanced)
	TArray<TObjectPtr<UMassEntityTraitBase>> Traits;
};
