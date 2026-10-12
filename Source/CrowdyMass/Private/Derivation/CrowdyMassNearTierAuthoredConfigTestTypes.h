#pragma once

#include "CoreMinimal.h"
#include "Representation/CrowdyMassRepresentationActorManagement.h"
#include "CrowdyMassNearTierAuthoredConfigTestTypes.generated.h"

/**
 * A game's own subclass of this plugin's representation actor management.
 *
 * It exists so that "a management class that already does what promotion needs is kept" can be told apart
 * from "the management class is always overwritten". Asserting with this plugin's own class in that slot
 * would pass either way, since keeping it and replacing it produce the same value, and a check that
 * cannot fail is not a check.
 *
 * Deliberately NOT wrapped in WITH_DEV_AUTOMATION_TESTS: UnrealHeaderTool refuses a reflected type inside
 * a preprocessor block, so this ships in game targets like its sibling fixture types do. Hidden because it
 * is the only one of them that derives from a live runtime class, which would otherwise offer it as a
 * choice in the representation actor management picker on every Mass visualization trait in the project.
 */
UCLASS(Hidden)
class UCrowdyMassNearTierAuthoredTestManagement : public UCrowdyMassRepresentationActorManagement
{
	GENERATED_BODY()
};
