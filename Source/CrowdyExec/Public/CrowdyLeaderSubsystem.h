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
 * The App's Server Object Leader: this client joins the Crowdy SDK's Leader type once it is signed in and the session
 * holds the actor id writing needs (a Player Derived avatar, a spectator or chat gives it one), and whoever leads publishes that actor id
 * and writes the replicated subsystems and the Leader-owned entities under it. The app's server code must be deployed for there to be a Leader.
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

	/** Whether this client is the Leader, and so writes the replicated subsystems: its user leads under this session's actor id. */
	UFUNCTION(BlueprintPure, Category = "Crowdy SDK|Leader")
	bool IsLocalLeader() const;

	/** The Leader changed; a user id of 0 means there is none. On the new Leader's own client it fires once that client can write as the Leader. */
	UPROPERTY(BlueprintAssignable, Category = "Crowdy SDK|Leader")
	FOnCrowdyLeaderChanged OnLeaderChanged;

#if WITH_DEV_AUTOMATION_TESTS
	/** Follows Object instead of acquiring the Leader type, and hands each Leader and its published actor id to WriteLeader instead of the game session. */
	void FollowForTest(UCrowdyServerObject* Object, UCrowdyGameSession* Session, TFunction<void(int64 LeaderUserID, const FGuid& LeaderActorID)> WriteLeader);

	/** What the SDK's sign-out broadcast does here. */
	void SignOutForTest() { HandleLogout(true); }

	/** What the SDK's sign-in broadcast resets here, without acquiring the Leader type again. */
	void SignInForTest()
	{
		ResetForSignIn();
		bRealtimeUp = false;
	}

	/** Runs the waiting no-actor-id check now instead of after its delay. False when none waits. */
	bool RunPlayerIDWarningForTest();

	/** The delay of the waiting retry, or 0 when none waits. Under FollowForTest a retry is only recorded, never ticked. */
	float GetPendingRetrySecondsForTest() const { return PendingRetrySeconds; }

	/** Runs the waiting retry now, as it runs once it acquires the same Leader object again. False when none waits. */
	bool RunRetryForTest();
#endif

	/** The realtime connection failed: exit, so the lead passes to a player who can still send. */
	UFUNCTION()
	void HandleConnectionLost();

	/** The realtime connection is up: enter, since only a player in the realtime world may lead. */
	UFUNCTION()
	void HandleConnectionUp();

private:
	void Acquire();
	void Follow(UCrowdyServerObject* Object);
	void Unfollow();
	/** Enters with this session's actor id; bAgain sends it even when that id was already accepted. */
	void Join(bool bAgain);
	void HandleEnterRefused(const FCrowdyServerCallResult& Result);
	FGuid FindLeaderActorID(int64 Leader) const;
	void AnnounceLeader();
	bool HasPlayerID() const;
	void WatchSession(UCrowdyGameSession* Session);
	/** One retry waits at a time; a shorter one replaces a longer. */
	void ArmRetry(float Seconds);
	void CancelRetry();
	void HandleStatusChanged(UCrowdyServerObject* Object, ECrowdyServerObjectStatus Status);
	void HandleValuesChanged(UCrowdyServerObject* Object, TConstArrayView<FString> Fields);
	void Publish(int64 NewLeader);
	void WriteHostStore() const;
	bool Retry(float DeltaTime);
	void ArmPlayerIDWarning();
	void CancelPlayerIDWarning();
	bool WarnIfStillNoPlayerID(float DeltaTime);
	void ResetForSignIn();

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
	TFunction<void(int64, const FGuid&)> WriteLeaderOverride;
	FDelegateHandle ValuesHandle;
	FTSTicker::FDelegateHandle RetryHandle;
	float PendingRetrySeconds = 0.f;
	int64 PublishedLeader = 0;
	/** The actor id PublishedLeader entered with; empty while it has none. */
	FGuid PublishedActorID;
	/** The Leader OnLeaderChanged last announced; behind PublishedLeader while this client leads but cannot write yet. */
	int64 AnnouncedLeader = 0;
	/** The actor id of the Enter in flight, so the same Enter is not sent twice. */
	FGuid EnteringActorID;
	/** The actor id of the last Enter the server accepted. */
	FGuid EnteredActorID;
	/** Counts sign-ins, so the answer to a call made before the latest one is ignored. */
	uint32 SignInEra = 0;
	bool bRealtimeUp = false;
	bool bWarnedFailure = false;
	/** Once per sign-in, so a refused Enter warns once rather than at every retry. */
	bool bWarnedEnter = false;
	/** The app's server code has no Enter, so only the slow retry enters, in case it was deployed again since. */
	bool bEnterStopped = false;
	/** Once per sign-in, so a client that never gets an actor id is told once rather than at every Leader push. */
	bool bWarnedPlayerID = false;
	/** Armed when joining first becomes possible with no actor id, so the moment before a pawn spawns warns nothing. */
	FTSTicker::FDelegateHandle PlayerIDWarningHandle;
};
