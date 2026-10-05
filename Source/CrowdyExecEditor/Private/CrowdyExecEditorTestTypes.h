#pragma once

#include "CoreMinimal.h"
#include "CrowdyExecEditorTestTypes.generated.h"

/** Kinds the client checks inside containers: names in a list, an optional GUID, and dates keyed by name. */
USTRUCT()
struct FCrowdyExecTestKinds
{
	GENERATED_BODY()

	UPROPERTY()
	TArray<FName> Names;

	UPROPERTY()
	TOptional<FGuid> MaybeId;

	UPROPERTY()
	TMap<FName, FDateTime> SeenAt;
};
