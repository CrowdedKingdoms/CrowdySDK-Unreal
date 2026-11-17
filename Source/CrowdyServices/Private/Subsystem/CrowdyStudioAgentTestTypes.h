#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyStudioAgentTestTypes.generated.h"

/** Counts the boot-changed events the Studio agent announces, as a Blueprint graph would receive them. */
UCLASS(Transient)
class UCrowdyStudioAgentTestListener : public UObject
{
	GENERATED_BODY()

public:
	int32 BootChanges = 0;

	UFUNCTION()
	void HandleBootChanged()
	{
		++BootChanges;
	}
};
