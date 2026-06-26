#pragma once
#include "CoreMinimal.h"
#include "EPullStateResult.generated.h"

/**
 * Result of an async PullState operation.
 * Expanded as exec output pins on the Pull State Blueprint node.
 */
UENUM(BlueprintType)
enum class EPullStateResult : uint8
{
	OnSuccess UMETA(DisplayName = "On Success"),
	OnFailure UMETA(DisplayName = "On Failure"),
};
