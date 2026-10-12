#include "Components/CrowdyMassEntityTraitsComponent.h"

UCrowdyMassEntityTraitsComponent::UCrowdyMassEntityTraitsComponent()
{
	// Authoring data read off the class default object, never off a spawned instance, so nothing here has
	// anything to do per frame. A crowd entity has no actor and therefore no component of this at all; the
	// class is what carries the answer.
	PrimaryComponentTick.bCanEverTick = false;
}
