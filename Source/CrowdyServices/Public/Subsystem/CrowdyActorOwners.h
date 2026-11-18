#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/Function.h"
#include "CrowdyActorOwners.generated.h"

class FCrowdyCppClient;
class UCrowdyGameSession;
struct FCrowdyCppJsonResult;

UENUM(BlueprintType)
enum class ECrowdyActorOwnerResult : uint8
{
	/** The platform's record for this id belongs to this user; a realtime sender can still use an id it does not own. */
	Confirmed,
	/** No record ties this id to a user. Name no one for it. */
	Unknown,
	/** The platform could not be asked. Ask again later. */
	Failed
};

USTRUCT(BlueprintType)
struct CROWDYSERVICES_API FCrowdyActorOwner
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Players")
	ECrowdyActorOwnerResult Result = ECrowdyActorOwnerResult::Unknown;

	/** The user the id is registered to; 0 unless Result is Confirmed. */
	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Players")
	int64 UserId = 0;

	UPROPERTY(BlueprintReadOnly, Category="Crowdy SDK|Players")
	FString Message;
};

DECLARE_DYNAMIC_DELEGATE_OneParam(FOnActorOwnerResolved, const FCrowdyActorOwner&, Owner);

/**
 * Which user the platform's record registers a remote actor id to. Confirmed says whose id it is, not who sent a
 * realtime message under it: the realtime layer does not check that a sender owns the id it uses. Also records the
 * signed-in player's own actor so other players can resolve it.
 */
UCLASS()
class CROWDYSERVICES_API UCrowdyActorOwners : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** How long an Unknown answer is reused before the platform is asked again. */
	static constexpr double UnknownSeconds = 10.0;

	/** The most Unknown answers kept; past it the ones expiring soonest are forgotten. */
	static constexpr int32 MaxUnknownEntries = 1024;

	/** The most actor ids one lookup request carries. */
	static constexpr int32 MaxIdsPerLookup = 50;

	/** How long a failed record of the player's own actor waits before it is tried again. */
	static constexpr float RecordRetrySeconds = 10.0f;

	/** The most times the player's own actor record is tried before it waits for the next id update. */
	static constexpr int32 MaxRecordTries = 3;

	/** The most fresh actor ids one sign-in mints because another player's record already holds the one it had. */
	static constexpr int32 MaxRemints = 3;

	/** How long sign-out waits for its actor record deletes before it lets the server session end anyway. */
	static constexpr float SignOutDeleteSeconds = 2.0f;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Answers once; inside this call only for an invalid id or an answer it already holds. Asks made in one tick share requests. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Players")
	void ResolveActorOwner(FGuid ActorId, FOnActorOwnerResolved OnResolved);

	void ResolveActorOwner(const FGuid& ActorId, TFunction<void(const FCrowdyActorOwner&)> OnResolved);

	/**
	 * Creates the record for the session's actor id afresh, then calls OnSettled once the create has answered: made,
	 * already this player's, or given up after its tries. An id another player's record holds is replaced and tried
	 * again first. OnSettled is kept for the sign-in and called again after a later replacement.
	 */
	void HandleSignedIn(TFunction<void()> OnSettled);

	/** True while a sign-in waits for its actor record's create, before which its realtime connection must not open. */
	bool IsSettlingSignIn() const { return bSignedIn && !bSettled; }

	/** True once a sign-in's actor record has answered, after which its realtime connection may open. */
	bool HasSettledSignIn() const { return bSignedIn && bSettled; }

	/**
	 * Deletes the actor records this sign-in made, then forgets what was learned and recorded for the account. Call it
	 * while the account's token is still installed. OnDeleted runs once the deletes answer, or after SignOutDeleteSeconds.
	 * A create still in flight is deleted under that token once it answers.
	 */
	void HandleSignedOut(TFunction<void()> OnDeleted);

	/** Stops a sign-out's wait for its deletes and runs its OnDeleted now; nothing when none waits. */
	void FinishSignOut();

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a collection, sending every call to Client. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId, UCrowdyGameSession* Session);

	void SetNowForTest(double Seconds) { NowForTest = Seconds; }

	/** Sends the queued lookups now instead of on the next tick. False when none is queued. */
	bool FlushLookupsForTest();

	/** Runs a waiting record retry now instead of after its delay. False when none waits. */
	bool RunRecordRetryForTest();

	int32 GetRecordWarningsForTest() const { return RecordWarningsForTest; }

	/** Ends a sign-out's wait for its deletes now instead of after SignOutDeleteSeconds. False when none waits. */
	bool RunSignOutDeadlineForTest();

	int32 GetUnknownCountForTest() const { return UnknownUntil.Num(); }
#endif

private:
	using FOwnerCallback = TFunction<void(const FCrowdyActorOwner&)>;

	UFUNCTION()
	void HandleOwnerUuidUpdated(FString Uuid);

	void WatchSession(UCrowdyGameSession* Session);
	void FlushLookups();
	void SendLookup(FCrowdyCppClient& Client, TArray<FGuid> ActorIds);
	void HandleLookup(const TArray<FGuid>& ActorIds, int64 AppId, const FCrowdyCppJsonResult& Result);
	void Remember(const FGuid& ActorId, const FCrowdyActorOwner& Owner);
	void AnswerWaiters(const FGuid& ActorId, const FCrowdyActorOwner& Owner);
	/** GameToken, when set, is the bearer for this one delete in place of the session's. */
	bool DeleteRecord(const FString& Uuid, TFunction<void()> OnAnswered = nullptr, const FString& GameToken = FString());
	void SendRecord();
	void HandleRecord(const FCrowdyCppJsonResult& Result);
	/** A create a sign-out left in flight, deleted under the sign-out's token if it lands. */
	struct FLateCreate
	{
		int32 Attempt = 0;
		int32 Generation = 0;
		FString Uuid;
		FString Token;
	};
	void HandleCreateAfterSignOut(const FLateCreate& Create, const FCrowdyCppJsonResult& Result);
	bool CanStillSend() const;
	void KeepRecord();
	void ReplaceSquattedId();
	void Settle();
	void HandleSignOutDelete(int32 Generation);
	void WarnRecord(const FString& Reason);
	void CancelRecordRetry();
	void CancelFlush();
	FCrowdyCppClient* ResolveClient() const;
	int64 GetAppId() const;
	double Now() const;

	TSharedPtr<uint8> LiveSessionToken;
	TWeakObjectPtr<UCrowdyGameSession> GameSession;
	TMap<FGuid, int64> ConfirmedOwners;
	TMap<FGuid, double> UnknownUntil;

	/** Everyone waiting on an id, whether it is queued for the next flush or already asked. */
	TMap<FGuid, TArray<FOwnerCallback>> Pending;
	TSet<FGuid> InFlight;
	FTSTicker::FDelegateHandle FlushHandle;

	/** Records are made only between HandleSignedIn and HandleSignedOut, so none is made under a token that is not the sign-in's. */
	bool bSignedIn = false;

	/** The user id whose actor is recorded or being recorded; 0 when none. */
	int64 RecordUserId = 0;
	FString RecordUuid;
	/** Whose the current record is, so sign-out never deletes one another player made. */
	bool bRecordIsOthers = false;
	/** This sign-in's previous record, deleted once the current one is made. */
	FString PreviousUuid;
	int32 RecordTries = 0;
	int32 Remints = 0;
	/** Counts every send and sign-out, so an answer to an older send is ignored. */
	int32 RecordAttempt = 0;
	/** The attempt of the create awaiting its answer; 0 when none is in flight. */
	int32 CreateInFlightAttempt = 0;
	FTSTicker::FDelegateHandle RecordRetryHandle;

	TArray<FLateCreate, TInlineAllocator<2>> LateCreates;

	/** Opens the realtime connection; called once per settled id. */
	TFunction<void()> OnRecordSettled;
	bool bSettled = false;

	TFunction<void()> OnSignOutDeleted;
	int32 SignOutDeletesLeft = 0;
	int32 SignOutGeneration = 0;
	FTSTicker::FDelegateHandle SignOutDeadlineHandle;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FCrowdyCppClient> ClientForTest;
	int64 AppIdForTest = 0;
	double NowForTest = 0.0;
	int32 RecordWarningsForTest = 0;
#endif
};
