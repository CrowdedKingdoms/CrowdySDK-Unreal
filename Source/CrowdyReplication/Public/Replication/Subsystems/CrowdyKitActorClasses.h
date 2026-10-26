#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UObject/SoftObjectPtr.h"

// The class each kind of Game Kit actor (mob, npc, pet, ball) is drawn as, from the Kit Actors of the project's starters. Game thread only.
namespace CrowdyKitActorClasses
{
	CROWDYREPLICATION_API void Set(FName Kind, const TSoftClassPtr<AActor>& Class);
	CROWDYREPLICATION_API void Reset();

	// The class for Kind, loaded if it is not; null, said once per kind and not loaded again, when no starter gives that kind a class that loads.
	CROWDYREPLICATION_API UClass* Resolve(FName Kind);
}
