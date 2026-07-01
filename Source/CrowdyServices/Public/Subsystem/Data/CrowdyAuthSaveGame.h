#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "CrowdyAuthSaveGame.generated.h"

/**
 * Persisted sign-in state. Only the identity SESSION token is stored. It is the
 * durable management-plane credential that mints app tokens. The app-scoped
 * GAMEPLAY token is short-lived (~30 min) and kept in memory only, never on disk.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyAuthSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY() FString SessionToken;
	UPROPERTY() int64 SessionGameTokenID = 0;
	UPROPERTY() int64 UserID = 0;
};
