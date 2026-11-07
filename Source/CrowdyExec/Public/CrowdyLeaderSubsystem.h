#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "CrowdyServerObjectTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "CrowdyLeaderSubsystem.generated.h"

class UCrowdyGameSession;
class UCrowdyServerObject;
class UCrowdySDKSubsystem;
class UCrowdyUDPSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCrowdyLeaderChanged, int64, NewLeaderUserID, int64, PreviousLeaderUserID);

/**
 * The App's Server Object Leader, under Leader Source Server Object: this client joins the Crowdy SDK's Leader type
 * once it is signed in with a Player Derived avatar, because the avatar gives it the player id writing needs, and
 * whoever leads writes the replicated subsystems. The Leader goes into the host store, so the
 * deprecated host functions report it too. The app's server code must be deployed for there to be a Leader. Under
 * Leader Source Elected Host it does nothing and there is no Leader.
 */
UCLASS(meta = (DisplayName = "Crowdy Leader Subsystem"))
class CROWDYEXEC_API UCrowdyLeaderSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The Leader's user id; 0 while there is none or it is not known yet. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Leader", meta = (DisplayName = "Get Leader User ID"))
	int64 GetLeaderUserID() const { return PublishedLeader; }

	/** Whether this client is the Leader, and so writes the replicated subsystems. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Leader")
	bool IsLocalLeader() const;

	/** The Leader changed; a user id of 0 means there is none. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Leader")
	FOnCrowdyLeaderChanged OnLeaderChanged;

#if WITH_DEV_AUTOMATION_TESTS
	/** Follows Object instead of acquiring the Leader type, and hands each Leader to WriteLeader instead of the host store. */
	void FollowForTest(UCrowdyServerObject* Object, UCrowdyGameSession* Session, TFunction<void(int64)> WriteLeader);
#endif

	/** The realtime connection failed: leave, so the lead passes to a player who can still send. */
	UFUNCTION()
	void HandleConnectionLost();

	/** The realtime connection is up: join, since only a player in the realtime world may lead. */
	UFUNCTION()
	void HandleConnectionUp();

private:
	void Acquire();
	void Follow(UCrowdyServerObject* Object);
	void Unfollow();
	void Join();
	bool HasPlayerID() const;
	void WatchSession(UCrowdyGameSession* Session);
	void ArmRetry();
	void HandleStatusChanged(UCrowdyServerObject* Object, ECrowdyServerObjectStatus Status);
	void HandleValuesChanged(UCrowdyServerObject* Object, TConstArrayView<FString> Fields);
	void Publish(int64 NewLeader);
	void WriteHostStore() const;
	void HandlePostLoadMap(UWorld* World);
	bool Retry(float DeltaTime);

	UFUNCTION()
	void HandleLogin(bool bSuccess, FString Message);

	UFUNCTION()
	void HandleLogout(bool bSuccess);

	UFUNCTION()
	void HandlePlayerIDSet(FString PlayerUUID);

	UPROPERTY()
	TObjectPtr<UCrowdyServerObject> LeaderObject;

	TWeakObjectPtr<UCrowdySDKSubsystem> SDKSubsystem;
	TWeakObjectPtr<UCrowdyUDPSubsystem> UdpSubsystem;
	TWeakObjectPtr<UCrowdyGameSession> GameSession;
	TFunction<void(int64)> WriteLeaderOverride;
	FDelegateHandle ValuesHandle;
	FTSTicker::FDelegateHandle RetryHandle;
	FDelegateHandle PostLoadMapHandle;
	int64 PublishedLeader = 0;
	bool bRealtimeUp = false;
	bool bWarnedFailure = false;
	bool bWarnedPlayerID = false;
};
