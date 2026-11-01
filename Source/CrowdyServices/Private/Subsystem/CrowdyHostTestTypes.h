#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "CrowdyHostTestTypes.generated.h"

/** Records what the host subsystem hands On Host Elected, as a Blueprint graph would receive it. */
UCLASS(Transient)
class UCrowdyHostTestListener : public UObject
{
	GENERATED_BODY()

public:
	TArray<FGuid> HostIDs;
	TArray<FGuid> PreviousHostIDs;

	UFUNCTION()
	void HandleHostElected(const FGuid& HostID, const FGuid& PreviousHostID)
	{
		HostIDs.Add(HostID);
		PreviousHostIDs.Add(PreviousHostID);
	}
};
