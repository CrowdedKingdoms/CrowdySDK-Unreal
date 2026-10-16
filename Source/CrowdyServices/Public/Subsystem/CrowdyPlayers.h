#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystem/CrowdyAccountTypes.h"
#include "CrowdyPlayers.generated.h"

class FCrowdyCppClient;

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnPlayerProfileRead, FCrowdyPlayerProfile, Profile);
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnPlayerProfilesRead, const TArray<FCrowdyPlayerProfile>&, Profiles);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnPlayersError, FCrowdyAccountError, Error, FString, Message);

/** Players' public names, for nametags and rosters, and renaming the signed-in player. */
UCLASS(BlueprintType, meta=(DisplayName="Crowdy Players"))
class CROWDYSERVICES_API UCrowdyPlayers : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** The most User IDs Get Player Names takes in one call. */
	static constexpr int32 MaxProfilesPerCall = 100;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Reads one player's name. A User ID with no such player fails. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Players")
	void GetPlayerName(int64 UserID, FOnPlayerProfileRead OnResult, FOnPlayersError OnError);

	/** Reads up to 100 players' names in one call. Unknown User IDs are left out of the answer. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Players")
	void GetPlayerNames(const TArray<int64>& UserIDs, FOnPlayerProfilesRead OnResult, FOnPlayersError OnError);

	/** Renames the signed-in player. Refused as Name Taken when another player has the same pair. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Players")
	void SetPlayerName(const FString& Gamertag, const FString& Disambiguation, FOnPlayerProfileRead OnResult,
		FOnPlayersError OnError);

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a subsystem collection, sending every call to Client. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client);
#endif

private:
	FCrowdyCppClient* ResolveClient() const;

	TSharedPtr<uint8> LiveToken;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FCrowdyCppClient> ClientForTest;
#endif
};
