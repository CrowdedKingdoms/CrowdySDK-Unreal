#pragma once

#include "CoreMinimal.h"
#include "Mass/CrowdyMassEntity.h"
#include "CrowdyMassEntityTestTypes.generated.h"

/**
 * A UCrowdyMassEntity subclass unrelated to UCrowdyMassCharacterAvatar, used only to prove that
 * UCrowdyMassEntitySubsystem::GetOrCreateAvatar refuses a cached avatar of the wrong class instead of
 * handing it back mismatched.
 */
UCLASS()
class UCrowdyMassOtherEntityTestReceiver : public UCrowdyMassEntity
{
	GENERATED_BODY()
};

/**
 * A concrete stand-in for "some participant that is not an avatar": a returning pawn, a world entity's own
 * object, or whatever already holds an id. It exists because UObject itself is abstract, so a test cannot
 * NewObject one to occupy a participant slot.
 */
UCLASS()
class UCrowdyMassOtherParticipantTestTarget : public UObject
{
	GENERATED_BODY()
};
