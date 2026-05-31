#pragma once

#include "CoreMinimal.h"
#include  "ECharacterType.generated.h"

UENUM(BlueprintType)
enum class ECharacterType : uint8
{
	SandboxCharacter UMETA(DisplayName = "SandboxCharacter"),
	MannyCharacter UMETA(DisplayName = "MannyCharacter"),
	QuinnCharacter UMETA(DisplayName = "QuinnCharacter"),
	UE4Character UMETA(DisplayName = "UE4Character"),
};