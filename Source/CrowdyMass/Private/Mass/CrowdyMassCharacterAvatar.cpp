#include "Mass/CrowdyMassCharacterAvatar.h"

#include "Fragments/CrowdyMassAnimFragments.h"
#include "Mass/CrowdyMassActionEvents.h"

void UCrowdyMassCharacterAvatar::PlayOneShot_Implementation(const int32 ActionId, const int32 Variant, const float PlayRate)
{
	WriteAction(ActionId, Variant, PlayRate);
}

void UCrowdyMassCharacterAvatar::HitReact_Implementation(const int32 ActionId, const int32 Variant)
{
	WriteAction(ActionId, Variant, 1.0f);
}

void UCrowdyMassCharacterAvatar::WriteAction(const int32 ActionId, const int32 Variant, const float PlayRate)
{
	FCrowdyMassActionFragment* Action = Frag<FCrowdyMassActionFragment>();
	if (!Action)
	{
		return;
	}

	// The same write a handler registered for a game's own actor-class action event performs, so an action
	// looks identical whichever road it travelled and the clamps cannot drift between the two.
	CrowdyMassWriteAction(*Action, ActionId, Variant, PlayRate);
}
