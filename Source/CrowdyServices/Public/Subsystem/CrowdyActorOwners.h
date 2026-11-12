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

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Answers once; inside this call only for an invalid id or an answer it already holds. Asks made in one tick share requests. */
	UFUNCTION(BlueprintCallable, Category="Crowdy SDK|Players")
	void ResolveActorOwner(FGuid ActorId, FOnActorOwnerResolved OnResolved);

	void ResolveActorOwner(const FGuid& ActorId, TFunction<void(const FCrowdyActorOwner&)> OnResolved);

	/** Forgets what was learned and recorded for the account that signed out. */
	void HandleSignedOut();

#if WITH_DEV_AUTOMATION_TESTS
	/** Stands in for Initialize on a subsystem built outside a collection, sending every call to Client. */
	void InitializeForTest(const TSharedPtr<FCrowdyCppClient>& Client, int64 AppId, UCrowdyGameSession* Session);

	void SetNowForTest(double Seconds) { NowForTest = Seconds; }

	/** Sends the queued lookups now instead of on the next tick. False when none is queued. */
	bool FlushLookupsForTest();

	/** Runs a waiting record retry now instead of after its delay. False when none waits. */
	bool RunRecordRetryForTest();

	int32 GetRecordWarningsForTest() const { return RecordWarningsForTest; }

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
	void SendRecord();
	void HandleRecord(const FCrowdyCppJsonResult& Result);
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

	/** The user id whose actor is recorded or being recorded; 0 when none. */
	int64 RecordUserId = 0;
	FString RecordUuid;
	int32 RecordTries = 0;
	/** Counts every send and sign-out, so an answer to an older send is ignored. */
	int32 RecordAttempt = 0;
	FTSTicker::FDelegateHandle RecordRetryHandle;

#if WITH_DEV_AUTOMATION_TESTS
	TWeakPtr<FCrowdyCppClient> ClientForTest;
	int64 AppIdForTest = 0;
	double NowForTest = 0.0;
	int32 RecordWarningsForTest = 0;
#endif
};
