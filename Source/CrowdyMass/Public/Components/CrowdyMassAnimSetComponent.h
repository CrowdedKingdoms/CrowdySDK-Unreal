#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"
#include "CrowdyMassAnimSetComponent.generated.h"

class UCrowdyMassAnimSet;

/**
 * Names the animation set entities of this actor's class render through on a client that draws them as
 * part of a crowd rather than as real actors.
 *
 * Add this to a character that has no Mass entity config authored for it. Such a class is drawn from a
 * config worked out from the class itself, and a class supplies a mesh but can supply no pose and gait
 * table, so without this the crowd stands in its bind pose. With it, that worked-out config gains an
 * animation trait naming this set and a mesh description carrying the set's Bank, and the crowd animates.
 *
 * A class that DOES have a Mass entity config authored for it normally names its set on the animation
 * trait there, and that wins. This component is still read for such a class when the config carries no
 * animation trait at all: a config authored for collision or level-of-detail reasons must not silently
 * mean "this class has no animation".
 *
 * The component holds no per-instance state and does nothing at runtime. It is read off the CLASS, before
 * anything is spawned, so the answer is the same for every actor of that class and is worked out once.
 */
UCLASS(ClassGroup=(CrowdyMass), meta=(BlueprintSpawnableComponent, DisplayName="Crowdy Mass Anim Set"))
class CROWDYMASS_API UCrowdyMassAnimSetComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCrowdyMassAnimSetComponent();

	/**
	 * The animation set this class's crowd representation renders through.
	 *
	 * Leaving it unset is reported as a class carrying the component with nothing in it, which is a
	 * different message from a class carrying no component at all: one is a slot to fill in and the other
	 * is a component to add.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy Mass|Anim")
	TSoftObjectPtr<UCrowdyMassAnimSet> AnimSet;
};
