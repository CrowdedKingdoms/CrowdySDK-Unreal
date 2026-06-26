#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "CrowdyAuthSaveGame.generated.h"

UCLASS()
class CROWDYSERVICES_API UCrowdyAuthSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY() FString GameToken;
	UPROPERTY() int64 GameTokenID = 0;
	UPROPERTY() int64 UserID = 0;
};
