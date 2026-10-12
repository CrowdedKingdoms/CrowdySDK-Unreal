#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"
#include "UObject/SoftObjectPtr.h"
#include "CrowdyMassNearTierShellComponent.generated.h"

class AActor;

/**
 * Names the actor an entity of this class is promoted to when it comes close enough to be drawn as a
 * real actor rather than as one row of a crowd.
 *
 * Add this to a character whose crowd representation should gain an actor at close range. Without it a
 * class is drawn as a crowd row at every distance, which is the behaviour every class has had until it
 * carries one: promotion is opt in, per class, and a class that declares no shell costs nothing.
 *
 * The class named here is a plain visual stand-in, not a second copy of the character. It is spawned
 * and released constantly as entities move in and out of range, so it should carry a mesh and the
 * things that decorate a mesh, and nothing that owns state or talks to the network.
 *
 * It must also carry a Crowdy Mass Near Tier component, which is what joins the spawned actor to the
 * entity and drives it. Crowdy Mass Near Tier Actor and its subclasses have one already; any other actor
 * class opts in by adding one. A class named here without one is refused, with a message saying so,
 * rather than promoted into an actor that would stand still where it appeared.
 *
 * The component holds no per-instance state and does nothing at runtime. It is read off the CLASS,
 * before anything is spawned, so the answer is the same for every actor of that class and is worked
 * out once.
 */
UCLASS(ClassGroup=(CrowdyMass), meta=(BlueprintSpawnableComponent, DisplayName="Crowdy Mass Near Tier Shell"))
class CROWDYMASS_API UCrowdyMassNearTierShellComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCrowdyMassNearTierShellComponent();

	/**
	 * The actor class entities of this class are promoted to at close range.
	 *
	 * Soft, because a class declaring a shell must not drag that shell's mesh into memory for every
	 * project that merely loads the class. It is loaded once, when a config is first worked out for the
	 * class, and never per entity.
	 *
	 * Leaving it unset is reported as a class carrying the component with nothing in it, which is a
	 * different message from a class carrying no component at all: one is a slot to fill in and the
	 * other is a component to add.
	 *
	 * Typed as an Actor rather than as this plugin's shell class so a game can promote to an actor class
	 * of its own. What is required of that class is a Crowdy Mass Near Tier component, not a particular
	 * base, and a class without one is refused with a message naming what is missing.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Crowdy Mass|Near Tier")
	TSoftClassPtr<AActor> NearTierActorClass;
};
